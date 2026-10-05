/*++
 * net1120_usb.c - x64 USBIO-compatible driver for NET_1120 USB Camera
 * VERSION 161 - PASSTHROUGH WITH HOLSTER-SAFE TIMEOUTS
 *
 * Two changes over v160 (see net1120_usb.h banner for details):
 *   1. WDF_REQUEST_SEND_OPTION_TIMEOUT (500 ms) on every URB submission.
 *   2. InterlockedExchange-gated double-completion guard in
 *      PassthroughUrbComplete.
 *
 * v160 architecture (unchanged): one URB per ReadFile, partial MDL into
 * DLL's pre-formatted buffer, USBD writes camera bytes directly there,
 * we copy IsoPacket results back into the DLL's table on completion.
 * Mirrors x86 sub_12B9A / sub_14DE4.
 --*/

#include "net1120_usb.h"
#include "net1120_init_table.h"
#include <ntstrsafe.h>

 // Stateful vendor-write pipeline (matches x86 driver's data[N] = wValue[N-1]).
static UCHAR   g_lastVendorWValue = 0;

// =========================================================================
// DriverEntry
// =========================================================================
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

// =========================================================================
// EvtDeviceAdd
// =========================================================================
NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    NTSTATUS status;
    WDFDEVICE device;

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_FILEOBJECT_CONFIG foCfg;
    WDF_FILEOBJECT_CONFIG_INIT(&foCfg, EvtDeviceFileCreate, EvtDeviceFileClose, WDF_NO_EVENT_CALLBACK);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &foCfg, WDF_NO_OBJECT_ATTRIBUTES);

    // Direct I/O: ReadFile output buffers come in as MDL-described, which is
    // what we pass to USBD via partial MDL.
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoDirect);

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, NET1120_DEVICE_EXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;

    static const GUID USBIO_GUID = {
        0x3f18f6fa, 0x5305, 0x4d2f,
        {0x93,0x38,0x1a,0x00,0xc0,0x06,0x53,0xdb}
    };
    WdfDeviceCreateDeviceInterface(device, &USBIO_GUID, NULL);

    // Parallel queue — many overlapped ReadFiles in flight simultaneously.
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = EvtIoDeviceControl;
    qcfg.EvtIoRead = EvtIoRead;
    WDFQUEUE queue;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) return status;

    PNET1120_DEVICE_EXT ext = GetDevExt(device);
    RtlZeroMemory(ext, sizeof(*ext));
    ext->WdfDevice = device;
    NET1120_KDPRINT(("USBIO: *** VERSION 161 PASSTHROUGH+TIMEOUT BUILD=" __DATE__ " " __TIME__ " ***\n"));

    KeInitializeSpinLock(&ext->StreamLock);
    ext->ActivePassthroughRequests = 0;
    ext->RequestTimeout = 1000;

    return STATUS_SUCCESS;
}

// =========================================================================
// EvtPrepareHardware / EvtReleaseHardware
// =========================================================================
NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Res, WDFCMRESLIST ResT)
{
    UNREFERENCED_PARAMETER(Res); UNREFERENCED_PARAMETER(ResT);
    NTSTATUS status;
    PNET1120_DEVICE_EXT ext = GetDevExt(Device);

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);

    WDF_USB_DEVICE_CREATE_CONFIG usbConfig;
    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&usbConfig, USBD_CLIENT_CONTRACT_VERSION_602);
    status = WdfUsbTargetDeviceCreateWithParameters(Device, &usbConfig, &attrs, &ext->UsbDevice);
    if (!NT_SUCCESS(status))
        status = WdfUsbTargetDeviceCreate(Device, &attrs, &ext->UsbDevice);
    if (!NT_SUCCESS(status)) return status;

    {
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS cfgParams;
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&cfgParams);
        NTSTATUS cfgStatus = WdfUsbTargetDeviceSelectConfig(
            ext->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &cfgParams);
        if (NT_SUCCESS(cfgStatus))
            ext->VideoInterface = cfgParams.Types.SingleInterface.ConfiguredUsbInterface;
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(ext->UsbDevice, &ext->DeviceDesc);
    return STATUS_SUCCESS;
}

NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResT)
{
    UNREFERENCED_PARAMETER(ResT);
    PNET1120_DEVICE_EXT ext = GetDevExt(Device);
    if (ext->UsbdHandle) { USBD_CloseHandle(ext->UsbdHandle); ext->UsbdHandle = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PrevState)
{
    UNREFERENCED_PARAMETER(Device); UNREFERENCED_PARAMETER(PrevState);
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PNET1120_DEVICE_EXT ext = GetDevExt(Device);
    UNREFERENCED_PARAMETER(TargetState);
    if (ext->Streaming) StopStreaming(ext);
    return STATUS_SUCCESS;
}

// =========================================================================
// WriteReg / ReadReg / InitDevice
// =========================================================================
NTSTATUS WriteReg(PNET1120_DEVICE_EXT ext, USHORT reg, UCHAR val)
{
    WDF_USB_CONTROL_SETUP_PACKET pkt;
    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
        &pkt, BmRequestHostToDevice, BmRequestToDevice, 0x01, val, reg);
    UCHAR buf = val;
    WDF_MEMORY_DESCRIPTOR mem;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, &buf, sizeof(buf));
    return WdfUsbTargetDeviceSendControlTransferSynchronously(
        ext->UsbDevice, NULL, NULL, &pkt, &mem, NULL);
}

NTSTATUS ReadReg(PNET1120_DEVICE_EXT ext, USHORT reg, PUCHAR val)
{
    WDF_USB_CONTROL_SETUP_PACKET pkt;
    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
        &pkt, BmRequestDeviceToHost, BmRequestToDevice, 0x00, 0, reg);
    UCHAR buf = 0;
    WDF_MEMORY_DESCRIPTOR mem;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, &buf, sizeof(buf));
    NTSTATUS status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        ext->UsbDevice, NULL, NULL, &pkt, &mem, NULL);
    if (NT_SUCCESS(status)) *val = buf;
    return status;
}

NTSTATUS InitDevice(PNET1120_DEVICE_EXT ext)
{
    for (ULONG i = 0; i < g_InitTableCount; i++) {
        NTSTATUS s = WriteReg(ext, g_InitTable[i].Reg, g_InitTable[i].Val);
        if (!NT_SUCCESS(s)) return s;
    }
    return STATUS_SUCCESS;
}

// =========================================================================
// StartStreaming - select alt 5, cache USBD pipe handle.
// No URB ring is started; URBs are submitted per ReadFile request.
// =========================================================================
NTSTATUS StartStreaming(PNET1120_DEVICE_EXT ext)
{
    if (ext->Streaming) return STATUS_SUCCESS;
    if (!ext->VideoInterface) return STATUS_DEVICE_NOT_CONNECTED;

    if (ext->UsbdHandle) { USBD_CloseHandle(ext->UsbdHandle); ext->UsbdHandle = NULL; }
    {
        PDEVICE_OBJECT wdmDev = WdfDeviceWdmGetDeviceObject(ext->WdfDevice);
        WDFIOTARGET    usbTarget = WdfUsbTargetDeviceGetIoTarget(ext->UsbDevice);
        PDEVICE_OBJECT wdmTarget = WdfIoTargetWdmGetTargetDeviceObject(usbTarget);
        NTSTATUS hs = USBD_CreateHandle(wdmDev, wdmTarget,
            USBD_CLIENT_CONTRACT_VERSION_602, 'BRU1', &ext->UsbdHandle);
        NET1120_KDPRINT(("USBIO: USBD_CreateHandle st=0x%x h=%p\n", hs, ext->UsbdHandle));
        if (!NT_SUCCESS(hs)) ext->UsbdHandle = NULL;
    }

    // x86 pcap: alt=0 (idle) then alt=5 (streaming) — properly resets pipe.
    {
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS idleParams;
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&idleParams, NET1120_VIDEO_ALT_IDLE);
        WdfUsbInterfaceSelectSetting(ext->VideoInterface, WDF_NO_OBJECT_ATTRIBUTES, &idleParams);
    }
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS altParams;
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&altParams, NET1120_VIDEO_ALT_STREAM);
    NTSTATUS status = WdfUsbInterfaceSelectSetting(
        ext->VideoInterface, WDF_NO_OBJECT_ATTRIBUTES, &altParams);
    if (!NT_SUCCESS(status)) {
        NET1120_KDPRINT(("USBIO: SelectAlt(%d) failed 0x%x\n", NET1120_VIDEO_ALT_STREAM, status));
        return status;
    }

    ext->IsochPipe = NULL;
    UCHAR numPipes = WdfUsbInterfaceGetNumConfiguredPipes(ext->VideoInterface);
    NET1120_KDPRINT(("USBIO: StartStreaming alt=%d numPipes=%d\n",
        NET1120_VIDEO_ALT_STREAM, numPipes));
    for (UCHAR i = 0; i < numPipes; i++) {
        WDF_USB_PIPE_INFORMATION pipeInfo;
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(ext->VideoInterface, i, &pipeInfo);
        NET1120_KDPRINT(("USBIO:   pipe[%d] ep=0x%02x type=%d mps=%d\n",
            i, pipeInfo.EndpointAddress, pipeInfo.PipeType, pipeInfo.MaximumPacketSize));
        if (pipeInfo.EndpointAddress == NET1120_EP_VIDEO_ISOCH &&
            pipeInfo.PipeType == WdfUsbPipeTypeIsochronous) {
            ext->IsochPipe = pipe;
        }
    }
    if (!ext->IsochPipe) {
        NET1120_KDPRINT(("USBIO: isoch pipe not found after SelectAlt\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ext->IsochPipeHandleWdm = WdfUsbTargetPipeWdmGetPipeHandle(ext->IsochPipe);
    ext->IsochTargetDevObj = WdfIoTargetWdmGetTargetDeviceObject(
        WdfUsbTargetPipeGetIoTarget(ext->IsochPipe));

    // x86 pcap: ABORT_PIPE before first isoch URB to clear stale pipe state.
    WdfUsbTargetPipeAbortSynchronously(ext->IsochPipe, WDF_NO_HANDLE, NULL);
    NET1120_KDPRINT(("USBIO: AbortPipe done\n"));

    ext->ActivePassthroughRequests = 0;
    ext->Streaming = TRUE;

    NET1120_KDPRINT(("USBIO: Streaming started (alt=5, passthrough mode)\n"));
    return STATUS_SUCCESS;
}

// =========================================================================
// StopStreaming - mark not streaming, abort pipe, drain in-flight URBs.
// =========================================================================
NTSTATUS StopStreaming(PNET1120_DEVICE_EXT ext)
{
    if (!ext->Streaming) return STATUS_SUCCESS;
    ext->Streaming = FALSE;
    ext->IsochPipeHandleWdm = NULL;
    ext->IsochTargetDevObj = NULL;

    // Abort the isoch pipe — every in-flight URB IRP completes with
    // STATUS_CANCELLED, our PassthroughUrbComplete then runs and completes
    // the corresponding WDFREQUEST.
    if (ext->IsochPipe)
        WdfUsbTargetPipeAbortSynchronously(ext->IsochPipe, WDF_NO_HANDLE, NULL);

    // Poll for completion routines to finish (max ~1 second).
    for (int spins = 0; ext->ActivePassthroughRequests > 0 && spins < 100; spins++) {
        LARGE_INTEGER tm; tm.QuadPart = -10 * 10 * 1000LL;  // 10 ms
        KeDelayExecutionThread(KernelMode, FALSE, &tm);
    }

    // Switch back to alt 0 (idle).
    if (ext->VideoInterface) {
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS altParams;
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&altParams, NET1120_VIDEO_ALT_IDLE);
        WdfUsbInterfaceSelectSetting(ext->VideoInterface, WDF_NO_OBJECT_ATTRIBUTES, &altParams);
    }
    ext->IsochPipe = NULL;

    return STATUS_SUCCESS;
}

// =========================================================================
// PassthroughUrbComplete - WDF completion callback (DISPATCH_LEVEL).
//
// Mirrors x86 sub_14DE4: write IsoPacket[i] back to DLL's table, then
// complete the WDFREQUEST with the URB's TransferBufferLength.
// WDF auto-frees the URB (parented to Request); we free our partial MDL.
// =========================================================================
VOID PassthroughUrbComplete(WDFREQUEST Request, WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Target);
    PNET1120_RF_CONTEXT ctx = (PNET1120_RF_CONTEXT)Context;

    // Defensive: if Context is NULL we can't safely do anything.
    if (!ctx) {
        return;
    }

    // V161 BSOD GUARD: ensure we only run the completion path once per request.
    // BSOD 0x3B (dump 050226-24515-01) showed FxRequest::CompleteInternal
    // dereferencing a NULL m_Irp, indicating WdfRequestComplete was being
    // called on an already-completed request during graphedt.exe cleanup
    // when many in-flight (holster-stalled) URBs were cancelled in a storm.
    // InterlockedExchange returns the prior value; if it's already 1 someone
    // beat us to it and we must not call WdfRequestCompleteWithInformation.
    if (InterlockedExchange(&ctx->Completed, 1) != 0) {
        NET1120_KDPRINT(("USBIO: PassthroughUrbComplete RE-ENTRY blocked req=%p\n",
            Request));
        return;
    }

    PNET1120_DEVICE_EXT ext = ctx->Ext;
    PURB urb = ctx->Urb;
    NTSTATUS irpStatus = Params->IoStatus.Status;
    ULONG byteCount = 0;

    // Write IsoPacket[i] results back into the DLL's pre-formatted table.
    if (urb && ctx->MappedBuffer && ctx->NumPackets > 0 &&
        urb->UrbHeader.Function == URB_FUNCTION_ISOCH_TRANSFER) {
        PULONG table = (PULONG)(ctx->MappedBuffer + RF_HDR_BYTES);
        for (ULONG i = 0; i < ctx->NumPackets; i++) {
            ULONG isoOff = urb->UrbIsochronousTransfer.IsoPacket[i].Offset;
            ULONG isoLen = urb->UrbIsochronousTransfer.IsoPacket[i].Length;
            USBD_STATUS isoSt = urb->UrbIsochronousTransfer.IsoPacket[i].Status;

            table[i * 3 + 0] = ctx->BaseOffset + isoOff;
            table[i * 3 + 1] = isoLen;
            table[i * 3 + 2] = (ULONG)isoSt;
        }
        // x86 sub_14DE4 returns URB.TransferBufferLength as IRP byte count for
        // isoch (line `*(_DWORD *)(v8 + 28) = *(_DWORD *)(v4 + 24)` in case 10).
        byteCount = urb->UrbIsochronousTransfer.TransferBufferLength;
    }

    if (ctx->PartialMdl) { IoFreeMdl(ctx->PartialMdl); ctx->PartialMdl = NULL; }

    InterlockedDecrement(&ext->ActivePassthroughRequests);

    // Translate result.
    //   STATUS_SUCCESS                   -> success
    //   STATUS_IO_TIMEOUT (V161)         -> success-with-zero-bytes; the URB
    //                                      hit our 500 ms send timeout because
    //                                      no isoch frames arrived (camera in
    //                                      holster).  Report success so the
    //                                      DLL keeps trying; the per-packet
    //                                      lengths in the table are 0, which
    //                                      sub_1000256B treats as no-op slots.
    //   STATUS_USBD_BABBLE_DETECTED 0xC0000B00 -> data still valid; report success.
    //   STATUS_CANCELLED, others         -> propagate.
    NTSTATUS finalStatus;
    if (NT_SUCCESS(irpStatus) ||
        irpStatus == (NTSTATUS)0xC0000B00 /* babble */ ||
        irpStatus == STATUS_IO_TIMEOUT) {
        finalStatus = STATUS_SUCCESS;
    }
    else {
        finalStatus = irpStatus;
    }

    WdfRequestCompleteWithInformation(Request, finalStatus, byteCount);
}

// =========================================================================
// SubmitPassthroughRead - per-request URB submission (x86 sub_12B9A analog).
//
// Reads the DLL's pre-formatted header+table, builds an isoch URB pointing
// into the DLL's user buffer via partial MDL, sends via WDF.
// Returns STATUS_PENDING if request is queued; non-pending = caller completes.
// =========================================================================
NTSTATUS SubmitPassthroughRead(PNET1120_DEVICE_EXT ext, WDFREQUEST Request)
{
    if (!ext->Streaming || !ext->IsochPipe || !ext->IsochPipeHandleWdm)
        return STATUS_DEVICE_NOT_READY;

    PMDL origMdl = NULL;
    NTSTATUS status = WdfRequestRetrieveOutputWdmMdl(Request, &origMdl);
    if (!NT_SUCCESS(status) || !origMdl) {
        NET1120_KDPRINT(("USBIO: RetrieveMdl fail 0x%x\n", status));
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    }

    PUCHAR mappedBuf = (PUCHAR)MmGetSystemAddressForMdlSafe(origMdl, NormalPagePriority);
    if (!mappedBuf) return STATUS_INSUFFICIENT_RESOURCES;
    SIZE_T mappedLen = MmGetMdlByteCount(origMdl);
    if (mappedLen < RF_HDR_BYTES) return STATUS_BUFFER_TOO_SMALL;

    // Read DLL header.  sub_10002373 fills:
    //   header[0] = num_packets, header[1] = total_data, header[2,3] = 0
    //   table[i]  = { data_offset, pkt_size, status }
    PULONG hdr = (PULONG)mappedBuf;
    ULONG numPackets = hdr[0];
    if (numPackets == 0 || numPackets > NET1120_RF_MAX_PACKETS) {
        NET1120_KDPRINT(("USBIO: RF bad numPackets=%lu\n", numPackets));
        return STATUS_INVALID_PARAMETER;
    }
    if (mappedLen < (SIZE_T)RF_HDR_BYTES + (SIZE_T)numPackets * 12) {
        NET1120_KDPRINT(("USBIO: RF buf too small for table %Iu np=%lu\n", mappedLen, numPackets));
        return STATUS_BUFFER_TOO_SMALL;
    }

    PULONG table = (PULONG)(mappedBuf + RF_HDR_BYTES);
    ULONG baseOffset = table[0];
    if (baseOffset < RF_HDR_BYTES + numPackets * 12 || baseOffset >= mappedLen) {
        NET1120_KDPRINT(("USBIO: RF bad baseOffset=%lu np=%lu len=%Iu\n",
            baseOffset, numPackets, mappedLen));
        return STATUS_INVALID_PARAMETER;
    }

    // Allocate URB parented to the request — auto-freed when request completes.
    WDF_OBJECT_ATTRIBUTES urbAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&urbAttrs);
    urbAttrs.ParentObject = Request;
    WDFMEMORY urbMem = NULL;
    PURB urb = NULL;
    status = WdfUsbTargetDeviceCreateIsochUrb(
        ext->UsbDevice, &urbAttrs, numPackets, &urbMem, &urb);
    if (!NT_SUCCESS(status) || !urb) {
        NET1120_KDPRINT(("USBIO: CreateIsochUrb fail 0x%x\n", status));
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }

    // Build a partial MDL covering only the data area of the DLL's buffer.
    SIZE_T dataLen = mappedLen - baseOffset;
    PVOID partialStartVa = (PUCHAR)MmGetMdlVirtualAddress(origMdl) + baseOffset;
    PMDL partialMdl = IoAllocateMdl(partialStartVa, (ULONG)dataLen, FALSE, FALSE, NULL);
    if (!partialMdl) {
        WdfObjectDelete(urbMem);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoBuildPartialMdl(origMdl, partialMdl, partialStartVa, (ULONG)dataLen);

    // Fill URB header and isoch transfer struct.
    {
        ULONG urbSz = GET_ISO_URB_SIZE(numPackets);
        urb->UrbHeader.Length = (USHORT)urbSz;
        urb->UrbHeader.Function = URB_FUNCTION_ISOCH_TRANSFER;
    }
    urb->UrbIsochronousTransfer.PipeHandle = ext->IsochPipeHandleWdm;
    urb->UrbIsochronousTransfer.TransferFlags =
        USBD_TRANSFER_DIRECTION_IN | USBD_START_ISO_TRANSFER_ASAP;
    urb->UrbIsochronousTransfer.TransferBuffer = NULL;
    urb->UrbIsochronousTransfer.TransferBufferMDL = partialMdl;
    urb->UrbIsochronousTransfer.TransferBufferLength = (ULONG)dataLen;
    urb->UrbIsochronousTransfer.NumberOfPackets = numPackets;
    urb->UrbIsochronousTransfer.StartFrame = 0;
    urb->UrbIsochronousTransfer.ErrorCount = 0;

    // Per-packet IsoPacket[i].Offset is relative to the partial MDL's start,
    // so subtract baseOffset from each table entry's data_offset.  Length is
    // initialised from the DLL's pkt_size (= wMPS = 3072) so USBD knows the
    // maximum it may write per packet.  USBD overwrites Length on completion.
    for (ULONG i = 0; i < numPackets; i++) {
        ULONG entryOff = table[i * 3 + 0];
        ULONG entryLen = table[i * 3 + 1];

        if (entryOff < baseOffset || (SIZE_T)entryOff + entryLen > mappedLen ||
            entryLen > NET1120_ISOCH_MAX_PKT) {
            urb->UrbIsochronousTransfer.IsoPacket[i].Offset = 0;
            urb->UrbIsochronousTransfer.IsoPacket[i].Length = 0;
            urb->UrbIsochronousTransfer.IsoPacket[i].Status = 0;
            continue;
        }
        urb->UrbIsochronousTransfer.IsoPacket[i].Offset = entryOff - baseOffset;
        urb->UrbIsochronousTransfer.IsoPacket[i].Length = entryLen;
        urb->UrbIsochronousTransfer.IsoPacket[i].Status = 0;
    }

    // Allocate per-request context and stash bookkeeping.
    WDF_OBJECT_ATTRIBUTES ctxAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ctxAttrs, NET1120_RF_CONTEXT);
    PNET1120_RF_CONTEXT ctx = NULL;
    status = WdfObjectAllocateContext(Request, &ctxAttrs, (PVOID*)&ctx);
    if (!NT_SUCCESS(status) || !ctx) {
        IoFreeMdl(partialMdl);
        WdfObjectDelete(urbMem);
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }
    ctx->Ext = ext;
    ctx->Urb = urb;
    ctx->UrbMem = urbMem;
    ctx->PartialMdl = partialMdl;
    ctx->MappedBuffer = mappedBuf;
    ctx->NumPackets = numPackets;
    ctx->BaseOffset = baseOffset;
    ctx->Completed = 0;          // V161: gate flag, set to 1 by completion routine

    // Format request as USB submit URB targeted at the isoch pipe.
    status = WdfUsbTargetPipeFormatRequestForUrb(
        ext->IsochPipe, Request, urbMem, NULL);
    if (!NT_SUCCESS(status)) {
        IoFreeMdl(partialMdl);
        ctx->PartialMdl = NULL;
        // urbMem auto-freed because parented to Request which the caller will
        // complete with this status.
        return status;
    }

    WdfRequestSetCompletionRoutine(Request, PassthroughUrbComplete, ctx);

    InterlockedIncrement(&ext->ActivePassthroughRequests);

    // V161: Per-URB 500 ms timeout.  When the camera is in its holster the
    // sensor stops producing frames; without a timeout, every URB we submit
    // would sit forever inside USBD waiting for isoch packets that never
    // arrive.  When GraphEdit then closes its handle, dozens of in-flight
    // cancellations storm WDF simultaneously and (on Windows 10 26100 at
    // least) trip an FxRequest internal NULL deref.
    //
    // 500 ms is long enough that real frames at 30 fps (33 ms inter-frame)
    // never get falsely cancelled, but short enough that holster-stall
    // URBs rotate cleanly (~2/sec instead of accumulating).
    WDF_REQUEST_SEND_OPTIONS sendOpts;
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOpts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOpts, WDF_REL_TIMEOUT_IN_MS(500));

    BOOLEAN sent = WdfRequestSend(
        Request,
        WdfUsbTargetPipeGetIoTarget(ext->IsochPipe),
        &sendOpts);

    if (!sent) {
        NTSTATUS sendStatus = WdfRequestGetStatus(Request);
        InterlockedDecrement(&ext->ActivePassthroughRequests);
        IoFreeMdl(partialMdl);
        ctx->PartialMdl = NULL;
        NET1120_KDPRINT(("USBIO: WdfRequestSend failed 0x%x\n", sendStatus));
        return NT_SUCCESS(sendStatus) ? STATUS_UNSUCCESSFUL : sendStatus;
    }

    return STATUS_PENDING;
}

// =========================================================================
// EvtIoRead - DLL ReadFile entry point.
// =========================================================================
VOID EvtIoRead(WDFQUEUE Queue, WDFREQUEST Request, SIZE_T Length)
{
    UNREFERENCED_PARAMETER(Length);
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PNET1120_DEVICE_EXT ext = GetDevExt(device);

#if DBG
    static LONG g_ReadTotal = 0;
    LONG rTotal = InterlockedIncrement(&g_ReadTotal);
    if (rTotal <= 5 || (rTotal % 200) == 0)
        NET1120_KDPRINT(("USBIO: ReadFile #%ld len=%Iu streaming=%d\n",
            rTotal, Length, (int)ext->Streaming));
#endif

    if (!ext->Streaming && ext->Configured) {
        NTSTATUS s = StartStreaming(ext);
        if (!NT_SUCCESS(s)) {
            NET1120_KDPRINT(("USBIO: ReadFile StartStreaming failed 0x%x\n", s));
            WdfRequestComplete(Request, s);
            return;
        }
    }

    NTSTATUS status = SubmitPassthroughRead(ext, Request);
    if (status == STATUS_PENDING) return;  // request will complete asynchronously

    NET1120_KDPRINT(("USBIO: ReadFile Submit failed 0x%x\n", status));
    WdfRequestComplete(Request, status);
}

// =========================================================================
// EvtDeviceFileCreate / Close
// =========================================================================
VOID EvtDeviceFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(FileObject);
#if DBG
    static LONG g_HandleSeq = 0;
    LONG hseq = InterlockedIncrement(&g_HandleSeq);
    NET1120_KDPRINT(("USBIO: FileCreate #%ld fo=%p\n", hseq, FileObject));
#endif
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID EvtDeviceFileClose(WDFFILEOBJECT FileObject)
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PNET1120_DEVICE_EXT ext = GetDevExt(device);
    NET1120_KDPRINT(("USBIO: FileClose fo=%p streaming=%d\n", FileObject, (int)ext->Streaming));
    if (ext->Streaming) StopStreaming(ext);
    NET1120_KDPRINT(("USBIO: FileClose done\n"));
}

// =========================================================================
// EvtIoDeviceControl - USBIO IOCTL dispatch
// =========================================================================
VOID EvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
    SIZE_T OutLen, SIZE_T InLen, ULONG IoCode)
{
    UNREFERENCED_PARAMETER(OutLen); UNREFERENCED_PARAMETER(InLen);
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PNET1120_DEVICE_EXT ext = GetDevExt(device);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    PVOID inBuf = NULL, outBuf = NULL;
    SIZE_T inSz = 0, outSz = 0;

#if DBG
    {
        static LONG g_IoctlSeq = 0;
        LONG seq = InterlockedIncrement(&g_IoctlSeq);
        BOOLEAN isVendor = (IoCode == IOCTL_USBIO_CLASS_OR_VENDOR_IN ||
            IoCode == IOCTL_USBIO_CLASS_OR_VENDOR_OUT);
        if (!isVendor || seq <= 5 || seq >= 1500) {
            WDF_REQUEST_PARAMETERS params;
            WDF_REQUEST_PARAMETERS_INIT(&params);
            WdfRequestGetParameters(Request, &params);
            NET1120_KDPRINT(("USBIO: IOCTL #%ld code=0x%08lx inSz=%Iu outSz=%Iu\n",
                seq, IoCode,
                params.Parameters.DeviceIoControl.InputBufferLength,
                params.Parameters.DeviceIoControl.OutputBufferLength));
        }
    }
#endif

    if (IoCode != IOCTL_USBIO_CLASS_OR_VENDOR_IN)
        WdfRequestRetrieveInputBuffer(Request, 0, &inBuf, &inSz);
    WdfRequestRetrieveOutputBuffer(Request, 0, &outBuf, &outSz);

    switch (IoCode) {

    case IOCTL_USBIO_GET_DRIVER_INFO:
    {
        if (outSz < sizeof(USBIO_DRIVER_INFO)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        USBIO_DRIVER_INFO* di = (USBIO_DRIVER_INFO*)outBuf;
        di->wAPIVersion = 0x0230; di->wDrvVersion = 0x0229;
        di->dwSvnRevision = 1338; di->dwPad = 0;
        info = sizeof(USBIO_DRIVER_INFO);
        break;
    }

    case IOCTL_USBIO_GET_DEVICE_INFO:
    {
        // sub_10003484: bit 0x100000 in first DWORD = high-bw iso mode (mps=3072,
        // queue=64, ReadFile=197392 bytes) — required for the DLL to use the
        // ReadFile data path that this driver supports.
        if (outSz == 0) { status = STATUS_BUFFER_TOO_SMALL; break; }
        ULONG tmp[4] = { 0 };
        tmp[0] = sizeof(USBIO_DEVICE_INFO) | 0x00100000;
        tmp[1] = (ULONG)ext->DeviceDesc.idVendor | ((ULONG)ext->DeviceDesc.idProduct << 16);
        tmp[2] = (ULONG)ext->DeviceDesc.bcdDevice;
        tmp[3] = 0;
        ULONG copyLen = (ULONG)min(outSz, sizeof(tmp));
        RtlCopyMemory(outBuf, tmp, copyLen);
        info = copyLen;
        break;
    }

    case IOCTL_USBIO_ACQUIRE_DEVICE:
        ext->Acquired = TRUE;
        break;

    case IOCTL_USBIO_RELEASE_DEVICE:
        ext->Acquired = FALSE;
        if (ext->Streaming) StopStreaming(ext);
        break;

    case IOCTL_USBIO_SET_CONFIGURATION:
    {
        if (inSz < sizeof(USBIO_SET_CONFIGURATION)) { status = STATUS_INVALID_PARAMETER; break; }
        USBIO_SET_CONFIGURATION* sc = (USBIO_SET_CONFIGURATION*)inBuf;
        if (!ext->VideoInterface)
            ext->VideoInterface = WdfUsbTargetDeviceGetInterface(ext->UsbDevice, 0);
        if (!ext->VideoInterface) { status = STATUS_DEVICE_CONFIGURATION_ERROR; break; }
        ext->Configured = TRUE;
        ext->CurrentConfig = sc->bConfigurationIndex;
        status = InitDevice(ext);
        break;
    }

    case IOCTL_USBIO_UNCONFIGURE_DEVICE:
        if (ext->Streaming) StopStreaming(ext);
        ext->Configured = FALSE;
        break;

    case IOCTL_USBIO_GET_DEVICE_PARAMETERS:
    {
        if (outSz < sizeof(USBIO_DEVICE_PARAMETERS)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        USBIO_DEVICE_PARAMETERS* dp = (USBIO_DEVICE_PARAMETERS*)outBuf;
        dp->dwSize = sizeof(USBIO_DEVICE_PARAMETERS);
        dp->dwOptions = ext->DeviceOptions;
        dp->dwTimeout = ext->RequestTimeout;
        dp->dwMaxIsoPackets = RF_NUM_PKTS;
        info = sizeof(USBIO_DEVICE_PARAMETERS);
        break;
    }

    case IOCTL_USBIO_SET_DEVICE_PARAMETERS:
    {
        if (inSz < 8) { status = STATUS_INVALID_PARAMETER; break; }
        ULONG* dw = (ULONG*)inBuf;
        ULONG dwOptions = (inSz >= 8) ? dw[1] : 0;
        ULONG dwTimeout = (inSz >= 12) ? dw[2] : 1000;
#if DBG
        ULONG dwMaxIso = (inSz >= 16) ? dw[3] : 0;
        NET1120_KDPRINT(("USBIO: SET_DEV_PARAMS opt=0x%08lx to=%lu maxIso=%lu\n",
            dwOptions, dwTimeout, dwMaxIso));
#endif
        ext->DeviceOptions = dwOptions;
        ext->RequestTimeout = dwTimeout ? dwTimeout : 1000;
        // dwOptions bit 0x10000 was used by v158 to allocate a kernel/user
        // shared frame buffer.  Passthrough mode does not need this — the
        // DLL writes directly to its ReadFile buffer.  Accept as a no-op.
        break;
    }

    case IOCTL_USBIO_GET_PIPE_PARAMETERS:   // doubles as START_STREAMING for the DLL
    {
#if DBG
        UCHAR epAddr = (inSz >= 1) ? *(UCHAR*)inBuf : 0;
        NET1120_KDPRINT(("USBIO: GET_PIPE_PARAMS ep=0x%02x inSz=%Iu outSz=%Iu\n",
            epAddr, inSz, outSz));
#endif
        if (outSz >= sizeof(USBIO_PIPE_PARAMETERS)) {
            USBIO_PIPE_PARAMETERS* pp = (USBIO_PIPE_PARAMETERS*)outBuf;
            pp->dwSize = sizeof(USBIO_PIPE_PARAMETERS);
            pp->bPipeType = USBIO_PIPETYPE_ISOCH;
            pp->bEndpointAddress = NET1120_EP_VIDEO_ISOCH;
            pp->wMaxPacketSize = NET1120_ISOCH_MAX_PKT;  // 3072
            pp->bInterval = 1;
            pp->dwFlags = 0;
            pp->dwMaxTransferSize = RF_DATA_BYTES;
            pp->dwMaxNumberOfPackets = RF_NUM_PKTS;
            WdfRequestSetInformation(Request, sizeof(USBIO_PIPE_PARAMETERS));
        }
        status = StartStreaming(ext);
        break;
    }

    case 0x8094201c:  // BIND_PIPE
    {
        if (inSz < sizeof(USBIO_BIND_PIPE)) { status = STATUS_INVALID_PARAMETER; break; }
        ext->BoundEndpoint = ((USBIO_BIND_PIPE*)inBuf)->bEndpointAddress;
        NET1120_KDPRINT(("USBIO: BIND_PIPE ep=0x%02x\n", ext->BoundEndpoint));
        break;
    }

    case IOCTL_USBIO_UNBIND_PIPE:
        // Match real driver: returns 0 for the standard (non-shared-buffer) case.
        if (outSz >= 4) {
            *(ULONG*)outBuf = 0;
            WdfRequestSetInformation(Request, 4);
        }
        ext->BoundEndpoint = 0;
        break;

    case IOCTL_USBIO_SET_PIPE_PARAMETERS:   // DLL uses this as STOP_STREAMING
        if (ext->Streaming) StopStreaming(ext);
        break;

    case IOCTL_USBIO_RESET_PIPE:
        // x86 sub_13DD8 simply submits URB_FUNCTION_RESET_PIPE and returns when
        // USBD acks.  Synchronous reset; sub_10002CD2 wrapper proceeds to
        // ReadFile loop on return.
        if (ext->IsochPipe)
            WdfUsbTargetPipeResetSynchronously(ext->IsochPipe, WDF_NO_HANDLE, NULL);
        break;

    case IOCTL_USBIO_ABORT_PIPE:
        if (ext->IsochPipe)
            WdfUsbTargetPipeAbortSynchronously(ext->IsochPipe, WDF_NO_HANDLE, NULL);
        break;

    case IOCTL_USBIO_CLASS_OR_VENDOR_IN:
    case IOCTL_USBIO_CLASS_OR_VENDOR_OUT:
    {
        PIRP wdmIrp = WdfRequestWdmGetIrp(Request);
        PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(wdmIrp);
        PVOID vendorBuf; ULONG vendorSz;
        if (IoCode == IOCTL_USBIO_CLASS_OR_VENDOR_IN) {
            vendorBuf = wdmIrp->AssociatedIrp.SystemBuffer;
            vendorSz = stack->Parameters.DeviceIoControl.InputBufferLength;
        }
        else {
            vendorBuf = inBuf; vendorSz = (ULONG)inSz;
        }
        if (!vendorBuf || vendorSz < 16) { status = STATUS_INVALID_PARAMETER; break; }

        PUCHAR rawBuf = (PUCHAR)vendorBuf;
        // V145 layout (pcap-verified):
        //   [8-9]   = USB data-phase byte (current write's data)
        //   [14-15] = USB wValue (next write's value, pre-packed lookahead)
        //   [16-17] = USB wIndex = register address
        USHORT regAddr = (vendorSz >= 18) ? *(USHORT*)(rawBuf + 16) : 0;
        USHORT regVal = (vendorSz >= 16) ? *(USHORT*)(rawBuf + 14) : 0;
        BOOLEAN isRead = (IoCode == IOCTL_USBIO_CLASS_OR_VENDOR_IN);

#if DBG
        {
            UCHAR  regData = (vendorSz >= 9) ? rawBuf[8] : 0;
            static LONG g_VendorWriteLog = 0;
            if (InterlockedIncrement(&g_VendorWriteLog) <= 10)
                NET1120_KDPRINT(("USBIO: VENDOR_%s reg=0x%04x wVal=0x%04x data=0x%02x sz=%lu\n",
                    isRead ? "IN " : "OUT", regAddr, regVal, regData, vendorSz));
        }
#endif

        if (isRead) {
            UCHAR regReadData = 0;
            WDF_USB_CONTROL_SETUP_PACKET pkt;
            WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
                &pkt, BmRequestDeviceToHost, BmRequestToDevice, 0x00, 0, regAddr);
            WDF_MEMORY_DESCRIPTOR mem;
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, &regReadData, 1);
            WDF_REQUEST_SEND_OPTIONS opts;
            WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
            WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(1000));
            ULONG xferred = 0;
            status = WdfUsbTargetDeviceSendControlTransferSynchronously(
                ext->UsbDevice, NULL, &opts, &pkt, &mem, &xferred);
            if (NT_SUCCESS(status)) {
                g_lastVendorWValue = regReadData;
                PMDL mdl = NULL;
                if (NT_SUCCESS(WdfRequestRetrieveOutputWdmMdl(Request, &mdl)) && mdl) {
                    PVOID buf = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
                    if (buf) *(PUCHAR)buf = regReadData;
                }
                PIRP wdmIrp2 = WdfRequestWdmGetIrp(Request);
                if (wdmIrp2 && wdmIrp2->MdlAddress) {
                    PVOID buf = MmGetSystemAddressForMdlSafe(wdmIrp2->MdlAddress, NormalPagePriority);
                    if (buf) { __try { *(PUCHAR)buf = regReadData; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
                }
                info = 1;
            }
        }
        else {
            // V147 stateful pipeline: data[N] = wValue[N-1], seeded from camera reg=0x0000.
            UCHAR dataBuf = g_lastVendorWValue;
            g_lastVendorWValue = (UCHAR)regVal;

            WDF_USB_CONTROL_SETUP_PACKET pkt;
            WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
                &pkt, BmRequestHostToDevice, BmRequestToDevice, 0x01, regVal, regAddr);
            WDF_MEMORY_DESCRIPTOR dataMem;
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&dataMem, &dataBuf, sizeof(dataBuf));
            WDF_REQUEST_SEND_OPTIONS opts;
            WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
            WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(500));
            status = WdfUsbTargetDeviceSendControlTransferSynchronously(
                ext->UsbDevice, NULL, &opts, &pkt, &dataMem, NULL);
            if (NT_SUCCESS(status)) info = 1;
        }
        break;
    }

    case IOCTL_USBIO_GET_CONFIGURATION_INFO:  // 0x80942050
    {
        // sub_10002CFE expected layout (verified byte-for-byte from x86 capture):
        //   [4..7]   pipe_count
        //   [264+]   16-byte pipe entries
        if (outSz < 296) { status = STATUS_BUFFER_TOO_SMALL; break; }
        PUCHAR r = (PUCHAR)outBuf;
        RtlZeroMemory(r, min(outSz, (SIZE_T)776));
        *(PULONG)(r + 4) = 2;
        PULONG e0 = (PULONG)(r + 264);    // interrupt pipe
        e0[0] = 3;           e0[1] = 0x00400000;
        e0[2] = 0x05810002;  e0[3] = 0;
        PULONG e1 = (PULONG)(r + 280);    // isoch pipe
        e1[0] = USBIO_PIPETYPE_ISOCH;
        e1[1] = 0x00300000;
        e1[2] = 0x01820400;  e1[3] = 0;   // low16=1024(wMPS), byte[2]=0x82(ep)
        info = 296;
        NET1120_KDPRINT(("USBIO: GET_CONFIGURATION_INFO → 2 pipes\n"));
        break;
    }

    case IOCTL_USBIO_READ_ISO_PIPE:       // 0x809420a5 — same buffer layout as ReadFile
    {
#if DBG
        static LONG g_ReadIsoTotal = 0;
        LONG riTotal = InterlockedIncrement(&g_ReadIsoTotal);
        if (riTotal <= 5 || (riTotal % 200) == 0)
            NET1120_KDPRINT(("USBIO: READ_ISO #%ld streaming=%d\n",
                riTotal, (int)ext->Streaming));
#endif
        if (!ext->Streaming && ext->Configured) {
            status = StartStreaming(ext);
            if (!NT_SUCCESS(status)) break;
        }
        NTSTATUS s = SubmitPassthroughRead(ext, Request);
        if (s == STATUS_PENDING) return;   // completes asynchronously
        status = s;
        break;
    }

    case IOCTL_USBIO_RESET_DEVICE:
        if (ext->Streaming) StopStreaming(ext);
        break;

    case IOCTL_USBIO_CYCLE_PORT:
        break;

    case IOCTL_USBIO_GET_DEVICE_POWER_STATE:
    {
        if (outSz < sizeof(USBIO_DEVICE_POWER)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        USBIO_DEVICE_POWER* dp = (USBIO_DEVICE_POWER*)outBuf;
        dp->dwSize = sizeof(USBIO_DEVICE_POWER);
        dp->dwDevicePowerState = 0;
        info = sizeof(USBIO_DEVICE_POWER);
        break;
    }

    case IOCTL_USBIO_SET_DEVICE_POWER_STATE:
        break;

    case IOCTL_USBIO_GET_BANDWIDTH_INFO:
    {
        if (outSz < sizeof(USBIO_BANDWIDTH_INFO)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        USBIO_BANDWIDTH_INFO* bi = (USBIO_BANDWIDTH_INFO*)outBuf;
        bi->dwSize = sizeof(USBIO_BANDWIDTH_INFO);
        bi->dwTotalBandwidth = 48000000;
        bi->dwAllocatedBandwidth = 24576000;
        bi->dwRemainingBandwidth = 23424000;
        info = sizeof(USBIO_BANDWIDTH_INFO);
        break;
    }

    case IOCTL_USBIO_SET_FEATURE:
        info = 0;
        break;

    case IOCTL_USBIO_GET_CONFIGURATION:
        break;

    case IOCTL_USBIO_GET_DESCRIPTOR:
    {
        if (outSz < sizeof(USB_DEVICE_DESCRIPTOR)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        RtlCopyMemory(outBuf, &ext->DeviceDesc, sizeof(USB_DEVICE_DESCRIPTOR));
        info = sizeof(USB_DEVICE_DESCRIPTOR);
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}