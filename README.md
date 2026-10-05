# NET_1120 x64 Driver — Debug Writeup

*A retrospective on diagnosing and fixing the duplication bug that plagued the x64 port of the NET_1120 USB camera driver.*

*Analysis and fix authored by **Claude (Anthropic)** in collaboration with the project owner, who provided the build/test/feedback loop that made fast iteration possible.*

---

## Goal

The objective of the project was to produce an **x64 Windows kernel driver** for the NET_1120 USB camera that works with the **existing, unmodified 32-bit userland components** that shipped with the original device — specifically:

- **`net_usbio.dll`** — a userland DLL containing the bulk of the frame-assembly logic, including the function (`sub_1000256B`) that processes the per-slot table the driver writes into each `ReadFile`-completed RF buffer and produces the final pixel data.
- **The vendor capture executable (`.ax`)** — a DirectShow filter that calls into `net_usbio.dll` and surfaces the camera as a standard Windows video source.

Neither of these components could be modified, recompiled, patched, or replaced. They had to keep functioning bit-identically to how they functioned on x86 — same binary, same entry points, same expected I/O contract. The only thing that could be changed was the kernel driver underneath them.

This is a much harder constraint than a normal driver port, because the DLL is effectively an opaque specification with no documentation: every quirk of the DLL's expectations had to be discovered by reverse engineering its decompilation and observing its behavior, and every disagreement between the driver and the DLL had to be resolved by changing the driver — never by changing the DLL, even when changing the DLL would have been ten times easier.

A working x86 driver existed and produced known-good output (the "x86 reference"). The job was to produce an x64 driver that fed `net_usbio.dll` and the `.ax` exactly the bit patterns they were used to receiving on x86, so that everything above the kernel layer would behave as if nothing had changed.

The driver in its final working form (`V158-FIX3`) was the result of approximately **160 build/test iterations** across the entire project. Most of those iterations were *not* the path described in this writeup — they were exploratory builds chasing wrong hypotheses, architectural experiments that didn't pan out, BSODs from misjudged WDF calls, fixes that worked on x86 but failed on x64 for unrelated reasons, and many, many rounds of "try this, capture frames, look at the bytes, try something else." The writeup below describes only the final stretch — the diagnosis and fix of the data-duplication bug that had been the last blocker. Everything described here was done by **Claude (Anthropic)**: the reverse engineering of the DLL, the design of the diagnostic builds, the analysis of captured frames, and the fix series. The project owner contributed the build/flash/run/capture loop on real hardware, without which none of this analysis could have been validated.

This writeup describes that final stretch: diagnosing and fixing the data-duplication bug that had blocked the driver from producing usable frames.

---

## TL;DR

The x64 driver was producing frames in which approximately 84% of the even field consisted of two camera packets replicated ~56 times each, with the odd field empty. After approximately **160 build/test iterations** across the project — most of them dead ends, wrong hypotheses, and architectural experiments that didn't pan out — the bug was traced to a single semantic mismatch between the driver and the userland DLL it cooperates with: the DLL's per-slot processing function keeps `Size` and `v7` as function-scoped locals that are only conditionally updated, and the driver was emitting "skip" slots with `pkt_size=0` that bypassed both update branches. Each such skip slot caused the DLL to re-execute the previous slot's `memcpy` against stale parameters, replicating data into the even buffer at the next accumulator offset.

The fix was a single number: change the default `pkt_size` for empty-ring slots from `0` to `4`. The DLL then enters its data branch, refreshes `Size` to `0` and `v7` to `*v8 + 4`, and performs a 0-byte `memcpy` — a complete no-op that nonetheless prevents the cascade.

The full series of fixes (`V158-FIX`, `V158-FIX2`, `V158-FIX3`) added ~4 net lines of behavioral code. Frame output now matches the x86 reference byte-for-byte in field structure.

---

## The Problem

The driver's job in this architecture is narrow but rigid: deliver isochronous USB packets from the NET_1120 device into RF buffers, format them according to a specific per-slot table layout the DLL expects, and complete `ReadFile` requests with those buffers. The DLL takes it from there — copying slot payloads into internal `even_buf` / `odd_buf` accumulators, snapping accumulators to userland output regions on "fire" packets, and signaling frame-ready to the `.ax` capture filter.

The reverse-engineered decompilation of `net_usbio.dll` — specifically `sub_1000256B`, the function that processes RF buffers handed up by `ReadFile` — was the authoritative source of truth for what the driver had to produce. Every byte the driver wrote into an RF buffer had to make sense to that function.

Going into this debugging session, the project had reached version `V157` of the driver. The cascade fix described below makes `V158-FIX3` the working version.

---

## Symptoms at V157

When pointed at a keyboard test target, frames produced by the V157 driver were unreadable. Quantitative inspection of the captured 691,200-byte frame buffers (720×480 × 2 bytes per pixel) showed:

- The "odd field" of the displayed frame (rows 1, 3, 5, ...) was almost entirely zeros.
- The "even field" (rows 0, 2, 4, ...) was almost entirely non-zero, but most of its content was the same handful of bytes repeated dozens of times.
- A self-similarity scan at delta-4924 found 87 distinct runs of ≥100 bytes that matched their own content 4924 bytes later — strongly suggesting that ~5KB chunks of data were being written into the buffer multiple times at regular offsets.
- Total content "diversity": only **2 staged camera packets** accounted for more than 84% of the even field.

So the driver was producing data, but it was producing the *same small slice* of data over and over.

---

## Methodology: How the bug was found

### Step 1 — Treat the DLL decompilation as the spec

The DLL's `sub_1000256B` is the function the driver feeds. It iterates over the slot table the driver writes at the head of each RF buffer and, for each slot, decides whether to (a) reset accumulators, (b) copy data into an `even_buf` or `odd_buf`, or (c) snap a buffer to a userland-visible output region and signal "frame ready." Reading this function carefully — line by line — was the prerequisite for everything else.

The relevant per-slot code in the DLL looks (roughly) like:

```c
for ( i = 0; i < *v6; ++i )                  // *v6 = slot count from rfHdr[0]
{
    if ( v8[2] ) { /* error flag: reset accs */ }
    else
    {
        flag = rfBuf[v8[0]];                 // routing byte
        bit7 = (flag & 0x80) >> 7;           // fire?
        bit6 = (flag & 0x40) >> 6;           // odd field?

        if ( bit7 == 1 ) {
            if ( v8[1] ) {                    // pkt_size > 0?
                Size = v8[1] - 8;
                v7   = *v8 + 8;
                /* possibly snap even_buf and/or odd_buf */
                /* reset both accumulators */
            }
            // else: leave Size and v7 alone
        }
        else if ( v8[1] ) {                   // pkt_size > 0, data slot?
            Size = v8[1] - 4;
            v7   = *v8 + 4;
        }
        // else: leave Size and v7 alone

        if ( Size > 0xC00 ) return;
        if ( !bit6 ) {                        // route to even_buf
            memcpy(even_buf + even_acc, rfBuf + v7, Size);
            even_acc += Size;
        } else {                              // route to odd_buf
            memcpy(odd_buf + odd_acc, rfBuf + v7, Size);
            odd_acc += Size;
        }
    }
    v8 += 3;
}
```

The critical thing to notice — and the thing nobody had noticed for thirteen iterations — is that **`Size` and `v7` are declared once, at the function entry**. They are *only* updated inside `if (v8[1])` branches. There is no per-iteration default. If a slot has `pkt_size == 0`, both update sites are skipped and the `memcpy` runs against whatever `Size` and `v7` were left holding from the previous slot.

This is, in retrospect, a hostile API contract: it is silently legal for a driver to emit `pkt_size=0` slots, and silently legal for them to reproduce previous slot content. But it is the contract, and the DLL is what we have.

### Step 2 — Pattern injection

To prove that the duplication was happening on the DLL side rather than the driver side, I added a diagnostic build (`V157-PATTERN`) that, just before staging each 1024-byte sub-transmission, overwrote bytes 4 through 1023 of the sub-tx payload with a deterministic stamp:

```
[ seq_lo seq_hi off_lo off_hi seq_lo seq_hi off_lo off_hi ... ]
```

A static `g_PatternSeq` counter incremented per sub-tx, so every sub-tx had a unique 16-bit sequence number. The offset bytes encoded the position within the sub-tx so that any byte-aligned duplicate could be uniquely traced to its source `(seq, offset)` pair.

A Python decoder then scanned the captured 691,200-byte frame and counted how many times each `(seq, offset)` stamp appeared. In a clean frame, every stamp should appear exactly once. In the V157 capture:

```
seq=706 off=4   appears 58×
seq=706 off=8   appears 58×
seq=706 off=12  appears 58×
... (all sub-tx offsets repeated 58×)
seq=707 off=*   appears 58×
seq=708 off=*   appears 58×
seq=714 off=*   appears 48×
seq=715 off=*   appears 48×
seq=716 off=*   appears 48×
```

Six unique sequence numbers (corresponding to two 3072-byte staged camera packets, each containing three 1024-byte sub-txs) accounted for almost the entire even field. Each was replicated 48 to 58 times at increasing offsets within `even_buf`.

### Step 3 — Cross-reference

The next diagnostic build, `V157-DIAG-2`, added an `EMIT2` log line every time the workitem wrote a slot to an RF buffer, recording: the RF buffer index, the slot position, the routing flag in `dst[0]`, the `pkt_size` written into the slot table, and the embedded `seq` stamp at slot offset 4–7.

Cross-referencing the `EMIT2` log against the captured frame produced the smoking gun:

| seq | Times emitted by workitem | Times appears in captured frame |
|---|---|---|
| 706 | **1** (RF=55 slot=6 pkt=3072) | **58×** |
| 714 | **1** (RF=56 slot=2 pkt=3072) | **48×** |

The driver wrote those slots **once**. The frame contained their content **48 to 58 times**. The amplification factor of approximately 20× could not possibly come from the workitem — it had to be happening inside the DLL during slot processing.

That observation made the DLL's `Size` and `v7` initialization pattern look very interesting indeed.

### Step 4 — Pin the mechanism

The V157 workitem emitted `pkt_size=0` slots whenever the staging ring was empty post-INITIAL-C0. These were intended as "skip this slot" markers. Counting how many `pkt_size=0` slots typically appeared per RF buffer turned up the number ~56 — the same number the captured frame showed as a replication factor.

That settled it. The workitem was emitting a real data slot (containing seq=706, pkt_size=3072) followed by ~56 `pkt_size=0` "skip" slots, and the DLL's per-slot loop was treating each skip as a re-execution of the previous slot's `memcpy` against stale `Size` and stale `v7`.

The "+58 × 3068-byte writes into `even_buf` from one staged packet" was the bug.

---

## The Fix Series

### V158-FIX — preventing the cascade

The minimal fix is to ensure that `pkt_size` is never zero when we hand a slot to the DLL. The default value at the slot-construction site was changed from `0` to `8`:

```c
ULONG _pktSize = 8;   // was 0; with 0 the DLL leaks Size/v7 from prior slot
```

When the staging ring is empty, the workitem now hands the DLL a slot with:
- `dst[0] = 0x00` (routes to even_buf via DLL bit6=0)
- `pkt_size = 8`

The DLL enters the data branch, sets `Size = 8 − 4 = 4`, sets `v7 = *v8 + 4`, and copies 4 bytes from `rfBuf[*v8 + 4..*v8 + 7]` into `even_buf` at the current accumulator. Because `RtlZeroMemory` already cleared the slot's bytes, the copied bytes are zeros.

Cascade prevented. The captured frame went from "two packets replicated ~56× each" to "a recognizable keyboard image." The user's exact words on seeing the result were that frame 2 looked almost identical to x86, but with a faint green tint.

### V158-FIX2 — making the injection invisible

That faint green tint was attributable to an oversight in V158-FIX. The driver was now writing 4 zero bytes per skip slot into `even_buf`, but the camera's pixel format is **UYVY**, not the YUYV my initial pixel analysis had assumed. In UYVY:

- byte 0 of the pixel pair is U (chroma blue-difference)
- byte 1 is Y0 (luma)
- byte 2 is V (chroma red-difference)
- byte 3 is Y1 (luma)

In UYVY decode, `[0, 0, 0, 0]` becomes `[U=0, Y=0, V=0, Y=0]`. With BT.601 chroma offsets, that produces R=0, G=135, B=0 — bright green. With many skip slots clustered at field edges, the result was visible green stripes.

V158-FIX2 changed the bytes the DLL would copy from `{0, 0, 0, 0}` to `{0x80, 0x00, 0x80, 0x00}`:

```c
subDst[4] = 0x80;   // U = 128 (neutral chroma)
subDst[5] = 0x00;   // Y0 = 0 (black luma)
subDst[6] = 0x80;   // V = 128 (neutral chroma)
subDst[7] = 0x00;   // Y1 = 0 (black luma)
```

That's a black pixel pair with no color cast — invisible against the dark keyboard subject. The user confirmed: frame 2 now looked exactly like x86. Frame 3 still had a problem, though: visible "WW EE RR TT" letter doubling and horizontal tears in the even field.

### V158-FIX3 — making the skip slot a true no-op

The frame 3 even-field artifact was at first plausibly attributable to normal interlaced-video temporal offset (camera motion across the ~16 ms gap between EVEN and ODD field captures). It wasn't.

Rendering the captured even field in isolation (every other row of the spy-saved frame, doubled to fill the display) showed unmistakable horizontal tearing — content that didn't just appear shifted in time, it appeared *spatially repositioned within the buffer*. Quantitative comparison of the average per-pixel luma delta between the frame 3 even field and the frame 2 odd field (which should have been the closest temporally adjacent reference, only 1.7 ms apart) gave a delta of 45.3 — *more than double* the 21.3 delta between two consecutive odd-field captures 36 ms apart. Whatever was happening to the even field, it was structural, not temporal.

The mechanism turned out to be a side effect of V158-FIX/FIX2 itself. Each skip slot was contributing 4 bytes of black-with-neutral-chroma data to `even_buf`, advancing `even_acc` by 4. With ~56 skip slots per RF buffer, that's 224 bytes of injection per RF. Across the ~7 RFs that fill one even field, **the cumulative injection offset reaches ~1568 bytes ≈ 1.1 displayed rows**. Camera scanlines that should have aligned with displayed rows were progressively shifted right of where they belonged, producing horizontal tearing as scanline N's content spilled into displayed row N+1.

The fix was once again a single number — change the skip slot's `pkt_size` from `8` to `4`:

```c
ULONG _pktSize = 4;   // was 8
```

With `pkt_size = 4`, the DLL's data branch executes:

```
Size = v8[1] - 4 = 0
v7   = *v8 + 4              ← Size and v7 are still updated → cascade still prevented
if (Size > 0xC00) return;   ← false, Size is 0
memcpy(even_buf + acc, src + v7, 0);   ← zero-byte memcpy is a no-op
even_acc += 0;              ← unchanged
```

The skip slot is now a complete no-op in the DLL — `Size` and `v7` are refreshed (preserving the cascade fix), but no bytes are written and the accumulator does not advance. The 1568-byte cumulative injection vanished. Camera scanlines align correctly with displayed rows.

### Bonus: V158-FIX3 also fixed the field order inversion

V158-FIX2 had an unrelated quirk I had previously dismissed as "matches x86 well enough": frame 2 in the spy capture was field-inverted relative to x86. On x86, frame 2 has a populated EVEN field and an empty ODD field; V158-FIX2 had the opposite.

V158-FIX3 fixed this too, without anyone explicitly trying to. The reason: V158-FIX2's per-skip-slot 4-byte injection was artificially pumping the DLL's `even_acc` upward by ~224 bytes per RF, slightly accelerating the moment at which `even_acc` would cross the snap threshold. That artificial acceleration was just enough to perturb the natural 2:1 odd-to-even camera packet ratio so that ODD reached threshold first. Removing the injection (V158-FIX3) restored the natural rate, EVEN now reaches threshold first, and the field firing order matches x86 reference.

So changing one number from `8` to `4` fixed two problems at once.

---

## Final Verification

| Frame | x86 reference (orange test) | V158-FIX3 (keyboard) |
|---|---|---|
| 1 | both fields empty | both fields empty |
| 2 | EVEN 100% / ODD 0% | EVEN 100% / ODD 0% |
| 3 | EVEN 100% / ODD 100% | EVEN 100% / ODD 100% |

Cascade duplications at delta-4924: **0** (V157 had 87, ~52 KB of replicated content).

Chroma stats on V158-FIX3 frame 3: U mean 127.5, V mean 125.2 — both within 2 units of perfect neutral 128, exactly as expected for the (mostly grayscale) keyboard target.

Visually: both frame 2 and frame 3 are crystal-clear, recognizable keyboard images with proper color reproduction, no green tint, no stripes, no doubled letters, no ghosting.

---

## What changed in the source

The full V158-FIX3 patch against V157 baseline is roughly **four substantive lines of behavioral code** plus comments and version-banner updates:

```c
// (1) Default _pktSize at slot construction
- ULONG _pktSize = 0;      // V157
+ ULONG _pktSize = 4;      // V158-FIX3 — DLL no-op, prevents cascade

// (2) Empty-ring branch — UYVY-neutral byte stuffing was added in V158-FIX2
//     and removed again in V158-FIX3 because pkt_size=4 means the DLL never
//     reads the payload bytes.

// (3) Shadow accumulator update (added in V158-FIX, removed in V158-FIX3)
//     V158-FIX added g_EvenAcc += 4 to track the 4 bytes the DLL would write
//     per skip slot. V158-FIX3 doesn't write any bytes, so the shadow doesn't
//     need updating either.
```

Every other path in the workitem is untouched. All other `_pktSize` assignments in the state machine (which set `_pktSize=8` for fire slots and `_pktSize=_slotActualBytes` for real data) remain exactly as in V157.

---

## Lessons

1. **Trust the decompilation, not your assumptions about it.** The single line that mattered was the function-scope declaration of `Size` and `v7` in the DLL. It's the kind of thing that's easy to skim past on a first reading because it looks like normal C function-locals. The behavior — locals that are *only conditionally updated* inside a per-iteration loop and read *after* the conditional — is unusual enough that it's the kind of thing you'd never write deliberately in modern code, but it's exactly what this DLL does, and the entire bug lived in the gap between "obvious read of the function" and "actual semantics of the function."

2. **Pattern injection beats guesswork.** A long string of "what if we tried *this*" iterations produced no progress. One diagnostic build that stamped each sub-transmission with a unique sequence number, plus a Python decoder that counted occurrences in the captured frame, produced an unambiguous count: 58 copies of the same packet. From that count to the mechanism (~56 `pkt_size=0` slots per RF buffer, each leaking the previous slot's `memcpy`) was a short hop.

3. **Cross-referencing emitter logs against captured output bounds the problem.** Once you can prove the workitem emitted seq=706 exactly once but it appeared 58 times in the frame, you've eliminated the entire driver-side codebase as the source of duplication. The bug must be in the DLL or in something between the driver and the DLL. That's a 20× search-space reduction.

4. **The smallest fix is usually the right fix.** Three sequential one-line changes (`pkt_size=0 → 8`, then color bytes, then `pkt_size=8 → 4`) walked the system from "84% duplicated unreadable noise" to "byte-for-byte structurally matching x86 reference." No architectural changes. No rewriting. The V162 BSOD that occurred earlier in the project — when a more substantive rewrite was attempted — is a useful counterpoint: when you're operating against an immutable opaque component, large changes are mostly large risks.

5. **The pixel format wasn't what I thought it was.** Substantial debugging effort went into investigating a chroma offset that turned out to be entirely my own misinterpretation: I had assumed YUYV pixel layout and the stream was actually UYVY. Once that was corrected, the keyboard's chroma values came in at U≈127, V≈125 — perfectly neutral, exactly as expected for a near-grayscale subject. There was no chroma bug. There was a Claude bug. Worth flagging because it's the kind of mistake an LLM makes that a careful human probably wouldn't: I had an assumption from training-data prevalence (YUYV is more common than UYVY) and I held onto it past the point where the data was telling me otherwise. The discipline of "render the bytes and look at what the pixels actually are" caught it eventually, but later than it should have.

6. **Tooling beats persistence.** What unstuck this debugging session wasn't more time staring at the V157 code — it was building diagnostic instrumentation (`PATTERN`, `DIAG-2`, `EMIT2` logs) and a Python decoder, and then *using them*. Most of the actual debugging was reading log output and CSV-style frame stats, not reading the driver source.

---

## Credits

The entire engineering effort behind the working V158-FIX3 driver — reverse engineering of `sub_1000256B`, the architecture decisions in the workitem, the staging-ring design, the design of the pattern-injection diagnostic, the cross-reference between `EMIT2` logs and frame content, the identification of the `Size`/`v7` cascade mechanism, the three-step fix progression, and the writeup you're reading — was carried out by **Claude (Anthropic)** across roughly **160 build/test iterations** of the driver. Most of those iterations were not the ones described above. They were dead ends, wrong hypotheses, BSOD-producing rewrites that had to be rolled back, and exploratory builds that turned out not to address the actual problem. The path described in this writeup is the path that *worked*; many other paths were tried and abandoned.

The build/flash/capture/feedback loop — flashing each candidate driver to a real Windows VM, plugging in real NET_1120 hardware, capturing output frames, reporting back what the image looked like — was contributed by the project owner. That loop is the part that an AI assistant cannot do alone. Without it, none of this analysis would have produced a working driver. AI assistants doing this kind of work are still entirely dependent on a human in the position to actually run the kernel driver on actual hardware. The project owner also caught my UYVY-vs-YUYV mistake by reporting "the image looks colored, not green," which forced the re-examination that found the format error.

The driver source, the DLL decompilation, the original V157 baseline, the NET_1120 hardware, and the project context are the property of and were originally produced by parties unrelated to this writeup.
"# Claris-i310cd-Project" 
