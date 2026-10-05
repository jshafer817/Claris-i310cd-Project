# Claris i310cd — x64 Driver Project (NET_1120 / STK1160)

A from-scratch **x64 Windows kernel driver** for the NET_1120 USB intraoral camera (sold as the Claris i310cd), built so the camera's original **unmodified 32-bit userland software** keeps working on 64-bit Windows.

| | |
|---|---|
| Device | `USB\VID_0932&PID_0300` (MI_00), Syntek STK1160 USB video bridge |
| Video | 720×480 NTSC, interlaced, UYVY 4:2:2, ~30 fps |
| Original stack | `net1120_usb.sys` (x86 only, Thesycon USBIO v2.41) → `NET_USBIO.dll` → `netvcam.ax` (DirectShow) |
| Our stack | **our x64 `net1120_usb.sys`** → the same, untouched `NET_USBIO.dll` → the same, untouched `netvcam.ax` |
| Final version | **v160** (passthrough architecture), **v161** (holster-safe timeouts) |
| Duration | March – May 2026, ~160 driver builds |

---

## Why this was hard

The camera only shipped with a 32-bit driver. The vendor's DirectShow filter (`netvcam.ax`) and helper DLL (`NET_USBIO.dll`) contain all the frame-assembly logic, and they **could not be modified, patched, or recompiled**. The only part we were allowed to replace was the kernel driver underneath them.

That made the DLL an undocumented specification. Every byte our driver handed up had to look exactly like what the original x86 driver would have handed up, and the only way to learn what that was was to reverse engineer the DLL, the original driver, and the USB traffic.

---

## Tools and software used

**Capture and tracing**
- **USBPcap + Wireshark**: bus-level USB captures on the working x86 machine and later on x64. These were the ground truth that broke the project open twice.
- **API Monitor v2**: recorded the DLL's `DeviceIoControl` / `ReadFile` call sequence on 32-bit Windows (`test.apmx86`).
- **DebugView**: live driver `KdPrint` logs (the `JUSTIN-PC.log` files).
- **WinDbg**: analysis of bugcheck crash dumps.

**Reverse engineering**
- **IDA Pro with the Hex-Rays decompiler**, used on:
  - `NET_USBIO.dll`. The key function is `sub_1000256B`, the per-slot frame assembler; `sub_10002373` builds the read buffer.
  - the original x86 `net1120_usb.sys`. The key functions are `sub_12B9A` and `sub_14DE4`, the read and URB-completion path.
  - `netvcam.ax`.
- The **public Thesycon USBIO header** (`usbio_i.h`), used to name the `0x8094xxxx` IOCTL codes found in the original driver.
- The vendor **INF file**, which revealed the original device parameters (`MaxIsoPackets=512`, `ShortTransferOk`, `RequestTimeout`, …).

**Build and test**
- **Visual Studio + WDK 10.0.26100** (KMDF/WDF, `usbdex.lib`)
- **VMware Workstation** with USB passthrough as the first test bed. The VM was switched from xHCI to EHCI to get around a USB 3 controller validation crash.
- **Real hardware** (bare-metal Windows 10/11 x64) for the final validation
- **GraphEdit** (DirectShow graph editor) for live rendering
- **Test signing** during development

**Custom tooling written for the project**
- `usbio_frame_spy`: a C++ capture app that drives the DLL and dumps the assembled frames (`frame_N.bin`, 691,200 bytes each) plus PNG renders and logs.
- A 3-frame capture test program, used for quick open/capture/close cycles.
- Python analysis scripts (`struct`, NumPy, Pillow):
  - a hand-written **pcapng + USBPcap parser**, because scapy and tshark weren't available
  - an independent **UYVY → RGB renderer**
  - per-field fill and duplication analysis, band and row-shift detection, and luma/chroma statistics
  - a decoder for the pattern-injection diagnostic (below)
- ffmpeg, used to render frame dumps.

---

## Milestones

### 1. First attempt: a pure Kernel Streaming (AVStream) driver (March)
The plan was to write a native KS video-capture driver and skip the vendor software entirely. The packet-level protocol was taken from a USBPcap capture of the camera on 32-bit Windows.

- The driver got as far as showing up in GraphEdit as **"NET_1120 USB Camera"** with a Capture pin. `FilterCreate` fired and reported 720×480.
- It then stalled on `VFW_E_NO_TRANSPORT` (0x80040274) when connecting the pin. The fixes we tried included changing the INF class from Camera to Media and adjusting the pin descriptor and allocator framing.

**Pivot:** reading the vendor INF and the original driver showed that the real stack was a **Thesycon USBIO** generic driver plus a DLL plus a DirectShow filter. Re-implementing USBIO would let the vendor's own software do all the video work.

### 2. A USBIO-compatible x64 driver
- Decoded all of the `0x8094xxxx` USBIO IOCTL codes in the original `.sys` and matched them to the public USBIO header.
- Implemented the path the DLL actually uses:
  - `GET_VERSION`, `ACQUIRE_DEVICE`, `GET_DESCRIPTOR`, `SET_CONFIGURATION`
  - the vendor register writes that make up the camera's init sequence (`net1120_init_table.h`)
  - `BIND_PIPE`, `RESET_PIPE`, `READ_ISO_PIPE`, and `ReadFile`
- Getting it stable meant fixing a series of bugchecks:
  - `0x144` corrupted URB: WDF request formatting overwrote `UsbdDeviceHandle`. Switched to a WDM IRP path.
  - A system service exception in `ucx01000` (xHCI validation). Switched the VM to EHCI.
  - `0x7F` double fault, twice:
    - `KdPrint` in hot paths overflowed the stack. Removed it.
    - The isoch completion routine resubmitted URBs directly. Moved the resubmit to a DPC.
  - `USBD_IsochUrbAllocate` failures. Switched to `WdfUsbTargetDeviceCreateIsochUrb`.
- **v20:** switched to direct I/O (`WdfDeviceIoDirect`, MDL-based `ReadFile`). This produced the first sustained streaming, though the frames were green, and it still bluescreened on close.
- **v24–v28:** fixed hang-on-close (pending `RESET_PIPE` and `ReadFile` requests were never drained when the file closed) and `STATUS_CANCELLED` URB resubmit storms. Corrected the URB packet geometry using a decompile of the original `.sys`.

### 3. Learning the read-buffer contract (April)
The DLL hands the driver a buffer with this layout:
```
[0..15]    header   { num_pkts, total_bytes, 0, 0 }
[16..783]  table    64 × { data_offset, pkt_size, error }
[784..]    data     64 × 3072-byte packet slots
```
Inside `NET_USBIO.dll`, `sub_1000256B` walks this table. The first byte of each STK1160 packet is a flag byte:
- bit 6 selects the field: even buffer or odd buffer.
- bit 7 means "fire". It snaps the accumulated field into the shared frame buffer once roughly 344 KB has accumulated.

**v77–v83:** a staging-ring design that fed this table from our own continuously running URBs. These builds fixed:
- stutters of about 0.5–1.1 seconds
- a 2-slot repeating-tile ring-wrap bug
- STK1160 packet headers leaking into the pixel data

### 4. V157 → V158-FIX3: the "cascade" bug
In V157, about 84% of the even field consisted of two camera packets repeated over and over, and the odd field was empty.

- **Pattern injection** (`V157-PATTERN`): every 1024-byte sub-transfer was stamped with a unique sequence number. The frames were then decoded to count how many times each stamp appeared. One packet showed up **58 times**.
- **Cross-referencing emit logs** (`V157-DIAG-2`): the driver had written that packet only **once**. So the duplication was happening inside the DLL.
- **Root cause:** in `sub_1000256B`, the locals `Size` and `v7` are only updated when `pkt_size != 0`. Our "skip" slots had `pkt_size = 0`, so each one re-ran the previous slot's `memcpy`.
- **The fix took three steps:**
  - `pkt_size 0 → 8` stopped the duplication, but the image had a green tint.
  - Writing UYVY-neutral bytes fixed the color, but the image still tore.
  - `pkt_size 8 → 4` turned skip slots into a true zero-byte no-op.

**Result:** VMware frames matched the x86 reference in field structure. A side lesson: the pixel format was **UYVY**, not the more common YUYV I had been assuming.

### 5. Real hardware disagreed
On bare metal, V158-FIX3 had two problems:
- visible horizontal **bands**
- a **BSOD after about 10 minutes** of GraphEdit streaming

The BSOD was a `WDFREQUEST` cancel/completion race:
- **FIX4** removed a double completion but broke unplug.
- **FIX4-MIN** replaced it.
- **FIX5** moved `WdfRequestMarkCancelable` inside the queue lock.

**FIX6** halved the bands. It still was not acceptable, and declaring it "shippable" was a mistake.

### 6. USBPcap, x86 vs x64 on the same PC
We captured 30 seconds of the same scene, on the same port, with each driver. A custom parser compared the two captures.

- The camera sent **identical data** to both drivers: the same flag ratios, about 1.01 to 1 odd versus even, and the same packet-size distribution.
- The **request shape was different**. x86 asked for **64** isoch packets per URB; ours asked for 8, so about 56 of the 64 table slots were synthetic.
- FIX3+PKTS matched x86 on the wire, both in packet count and in the 8.000 ms URB cadence. **The bands were still there.**

### 7. v160: throw out the reconstruction, copy the x86 design (May)
A spy capture on a keyboard target showed the bands were **already in the frame buffer**. That pointed the blame at our slot reconstruction. Re-reading the decompile of the original `.sys` (`sub_12B9A` / `sub_14DE4`) showed the x86 driver never reconstructed anything:

1. The DLL pre-formats the read buffer with the header and packet table.
2. The driver builds **one URB per read**, with `NumberOfPackets = header[0]` and the per-packet offsets taken straight from the DLL's table. It uses a partial MDL that points into the DLL's own buffer.
3. USBD writes the camera bytes **directly** into the DLL's buffer.
4. On completion, the driver writes the actual packet lengths back into the table and completes the read.

v160 does exactly this. That deleted all of the following: the staging ring, the workitem, the shadow accumulators, synthetic fire packets, flag suppression and re-flagging, skip slots, and the shared kernel frame buffer. Cancellation is left entirely to WDF.

**Result on real hardware:** a clean image, no bluescreens, and the camera ejects cleanly.

### 8. v161: the holster
The camera stops sending frames while it sits in its holster. URBs then waited forever. When GraphEdit closed, the mass cancellation hit a NULL `m_Irp` inside WDF and caused a `0x3B` bugcheck. v161 makes two changes:
- a **500 ms per-URB timeout** (`WDF_REQUEST_SEND_OPTION_TIMEOUT`)
- an `InterlockedExchange` guard against completing the same request twice

---

## Lessons

1. **Match the original architecture before inventing your own.** Most of the 160 builds went into making a staging-and-reconstruction design imitate what the x86 driver did by simply passing the DLL's buffer through. The final driver is much smaller than the broken ones.
2. **Wire-level ground truth beats internal metrics.** The USBPcap comparison and the spy frame dumps settled questions that weeks of driver-internal statistics had not.
3. **A human looking at the picture is the real test.** Byte-uniqueness and band-count numbers repeatedly said "fixed" while the image was visibly broken. "It still looks bad" was right every time.
4. **VMware is not real hardware.** The VM's USB timing hid both the cancel race and the banding.
5. **Instrument, don't guess.** Pattern injection plus emit-log cross-referencing found the cascade bug in a few builds, after many builds of guessing.
6. **Small, reversible changes.** The larger rewrites (V162, FIX4) caused regressions. Each fix that worked was a few lines.
7. **Check your assumptions about data formats.** UYVY versus YUYV caused a phantom "chroma bug."

---

## Credits

- **Project owner (Justin):** hardware, the build/sign/install/test loop on VMware and bare metal across about 160 builds, the USBPcap and API Monitor captures, IDA sessions and decompile exports, the spy and test tools, and the visual judgment calls that repeatedly caught false "it's fixed" conclusions.
- **Claude (Anthropic):** driver code, reverse-engineering analysis of the decompiles, diagnostic build design, Python analysis tooling, and hypotheses. Several of those hypotheses were wrong along the way, and the lessons above reflect that.

---

## Notes

- `NET_USBIO.dll`, `netvcam.ax`, and the original x86 `net1120_usb.sys` are proprietary to their owners (NET GmbH / Thesycon). They are **not** included in this repository. This project only replaces the kernel driver, for interoperability on x64 Windows.
- Future direction under discussion: a native **AVStream** minidriver that does field assembly in the kernel, so the camera works in DirectShow and Media Foundation without the vendor DLL or `.ax`.
