/*++
 * net1120_usb.h - x64 USBIO-compatible driver for NET_1120 camera
 * VERSION 161 - PASSTHROUGH ARCHITECTURE WITH HOLSTER-SAFE TIMEOUTS
 *
 *   v161 changes (over v160):
 *     - Per-URB timeout (500 ms) on WdfRequestSend so stalled URBs (camera
 *       in holster, sensor parked) do not accumulate.  Without this, every
 *       overlapped ReadFile from the DLL queues a URB that USBD never
 *       completes; on file close, dozens of in-flight cancellations storm
 *       the completion path and trip a WDF FxRequest::CompleteInternal NULL
 *       deref (BSOD 0x3B / SYSTEM_SERVICE_EXCEPTION, dump 050226-24515-01).
 *     - Cancel-safe completion gate via InterlockedExchange on a new
 *       Completed flag in the RF context.  If WDF somehow re-enters our
 *       completion routine for the same request (driver-verifier-class
 *       defensive fix), the second entry returns without calling
 *       WdfRequestCompleteWithInformation.
 *     - Streaming flag re-checked at completion time so we do not write
 *       packet table into a buffer that has been torn down.
 *
 *   v160 architecture (unchanged):
 *
 *   Device: USB\VID_0932&PID_0300, STK1160 intraoral camera
 *   Goal:   x64 driver speaking USBIO v2.41 so unmodified x86
 *           NET_USBIO.dll + netvcam.ax work on x64 Windows.
 *
 *     Each IRP_MJ_READ / READ_ISO_PIPE comes in with a buffer the DLL has
 *     pre-formatted with a header + per-packet table (sub_10002373 in
 *     NET_USBIO.dll fills it).  We allocate a URB sized for header[0]
 *     packets, build a partial MDL into the DLL's user buffer at base
 *     offset 784, and submit the URB with TransferBufferMDL pointing at
 *     it.  USBD writes camera bytes directly into the DLL's buffer at
 *     the offsets the DLL specified.  On completion we write
 *     IsoPacket[i].Length back to table[i].pkt_size and complete the
 *     WDFREQUEST; the DLL's slot processor sees the camera byte stream
 *     verbatim, the same way it does on x86.
 *
 *     Mirrors x86 sub_12B9A (submit) and sub_14DE4 (complete) in
 *     net1120.sys decompile.
 --*/
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <wdfusb.h>

#define USBIO_DEVICE_TYPE   0x8094

#define IOCTL_USBIO_GET_DESCRIPTOR          CTL_CODE(0x8094, 0x801, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_USBIO_SET_DESCRIPTOR          CTL_CODE(0x8094, 0x802, METHOD_IN_DIRECT,  FILE_ANY_ACCESS)
#define IOCTL_USBIO_SET_FEATURE             CTL_CODE(0x8094, 0x803, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_CLEAR_FEATURE           CTL_CODE(0x8094, 0x804, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_CONFIGURATION       CTL_CODE(0x8094, 0x806, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_STORE_CONFIG_DESC       CTL_CODE(0x8094, 0x808, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_SET_CONFIGURATION       CTL_CODE(0x8094, 0x809, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_UNCONFIGURE_DEVICE      CTL_CODE(0x8094, 0x80a, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_SET_DEVICE_PARAMETERS   CTL_CODE(0x8094, 0x80b, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_CLASS_OR_VENDOR_IN      CTL_CODE(0x8094, 0x80c, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_USBIO_CLASS_OR_VENDOR_OUT     CTL_CODE(0x8094, 0x80d, METHOD_IN_DIRECT,  FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_DEVICE_PARAMETERS   CTL_CODE(0x8094, 0x80f, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_SET_DEVICE_PARAMETERS2  CTL_CODE(0x8094, 0x810, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_CONFIGURATION_INFO  CTL_CODE(0x8094, 0x814, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_RESET_DEVICE            CTL_CODE(0x8094, 0x815, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_SET_DEVICE_POWER_STATE  CTL_CODE(0x8094, 0x817, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_DEVICE_POWER_STATE  CTL_CODE(0x8094, 0x818, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_BANDWIDTH_INFO      CTL_CODE(0x8094, 0x819, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_DEVICE_INFO         CTL_CODE(0x8094, 0x81a, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_DRIVER_INFO         CTL_CODE(0x8094, 0x81b, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_CYCLE_PORT              CTL_CODE(0x8094, 0x81c, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_GET_PIPE_PARAMETERS     CTL_CODE(0x8094, 0x81e, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_SET_PIPE_PARAMETERS     CTL_CODE(0x8094, 0x81f, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_RESET_PIPE              CTL_CODE(0x8094, 0x820, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_ABORT_PIPE              CTL_CODE(0x8094, 0x821, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_UNBIND_PIPE             CTL_CODE(0x8094, 0x823, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_ACQUIRE_DEVICE          CTL_CODE(0x8094, 0x832, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_RELEASE_DEVICE          CTL_CODE(0x8094, 0x833, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_USBIO_READ_ISO_PIPE           CTL_CODE(0x8094, 0x829, METHOD_IN_DIRECT,  FILE_ANY_ACCESS)

#pragma pack(push,1)

#if DBG
#define NET1120_KDPRINT(_x_)   KdPrint(_x_)
#else
#define NET1120_KDPRINT(_x_)   ((void)0)
#endif

typedef struct _USBIO_DRIVER_INFO {
    USHORT wAPIVersion; USHORT wDrvVersion; ULONG dwSvnRevision; ULONG dwPad;
} USBIO_DRIVER_INFO;

typedef struct _USBIO_DEVICE_INFO {
    ULONG dwSize; USHORT idVendor; USHORT idProduct; USHORT bcdDevice;
    UCHAR bDeviceClass; UCHAR bDeviceSubClass; UCHAR bDeviceProtocol;
    UCHAR bNumConfigurations; USHORT wNumLanguageIDs;
    UCHAR bManufacturerStringIndex; UCHAR bProductStringIndex; UCHAR bSerialNumberStringIndex;
} USBIO_DEVICE_INFO;

typedef struct _USBIO_SET_CONFIGURATION {
    ULONG dwSize; UCHAR bConfigurationIndex; ULONG dwFlags;
} USBIO_SET_CONFIGURATION;

typedef struct _USBIO_BIND_PIPE {
    ULONG dwSize; UCHAR bEndpointAddress;
} USBIO_BIND_PIPE;

typedef struct _USBIO_PIPE_PARAMETERS {
    ULONG dwSize; UCHAR bPipeType; UCHAR bEndpointAddress; USHORT wMaxPacketSize;
    UCHAR bInterval; ULONG dwFlags; ULONG dwMaxTransferSize; ULONG dwMaxNumberOfPackets;
} USBIO_PIPE_PARAMETERS;

#define USBIO_PIPETYPE_CONTROL   0
#define USBIO_PIPETYPE_ISOCH     1
#define USBIO_PIPETYPE_BULK      2
#define USBIO_PIPETYPE_INTERRUPT 3

typedef struct _USBIO_CLASS_OR_VENDOR_REQUEST {
    ULONG dwSize; ULONG dwFlags; UCHAR bRequestType; UCHAR bRequest;
    USHORT wValue; USHORT wIndex; USHORT wLength; USHORT regValue; USHORT regAddr;
} USBIO_CLASS_OR_VENDOR_REQUEST;

#define USBIO_RECIPIENT_DEVICE    0x00000000
#define USBIO_RECIPIENT_INTERFACE 0x00000001
#define USBIO_RECIPIENT_ENDPOINT  0x00000002
#define USBIO_REQUEST_VENDOR      0x00000020

typedef struct _USBIO_DEVICE_PARAMETERS {
    ULONG dwSize; ULONG dwOptions; ULONG dwTimeout; ULONG dwMaxIsoPackets;
} USBIO_DEVICE_PARAMETERS;

typedef struct _USBIO_DEVICE_POWER {
    ULONG dwSize; ULONG dwDevicePowerState;
} USBIO_DEVICE_POWER;

typedef struct _USBIO_BANDWIDTH_INFO {
    ULONG dwSize; ULONG dwTotalBandwidth; ULONG dwAllocatedBandwidth; ULONG dwRemainingBandwidth;
} USBIO_BANDWIDTH_INFO;

typedef struct _USBIO_DESCRIPTOR_REQUEST {
    ULONG dwSize; UCHAR Recipient; UCHAR bType; UCHAR bIndex; USHORT wLanguageID;
} USBIO_DESCRIPTOR_REQUEST;

typedef struct _USBIO_FEATURE_REQUEST {
    ULONG dwSize; UCHAR Recipient; USHORT wFeatureSelector; USHORT wIndex;
} USBIO_FEATURE_REQUEST;

#pragma pack(pop)

// -------------------------------------------------------------------------
// Camera parameters (pcap-verified)
// -------------------------------------------------------------------------
#define NET1120_VENDOR_ID         0x0932
#define NET1120_PRODUCT_ID        0x0300
#define NET1120_EP_VIDEO_ISOCH    0x82
#define NET1120_EP_INTERRUPT      0x81
#define NET1120_VIDEO_IFACE       0
#define NET1120_VIDEO_ALT_IDLE    0
#define NET1120_VIDEO_ALT_STREAM  5     // MPS=1024 mult=3 = 3072 B/microframe (pcap-verified)
#define NET1120_ISOCH_MAX_PKT     3072  // wMaxPacketSize for the isoch pipe
#define NET1120_RF_MAX_PACKETS    256   // sanity cap on header[0] from DLL (DLL uses 64)

#define USBIO_API_VERSION_MAJOR   0x02
#define USBIO_API_VERSION_MINOR   0x30

// ReadFile buffer geometry (NET_USBIO.dll sub_10002373):
//   header[0]   = num_packets         (= 64)
//   header[1]   = total_data_bytes    (= 196608 = 64 * 3072)
//   header[2,3] = 0
//   table[i]    = { data_offset, pkt_size, status }   for i=0..63
//   data area starts at table[0].data_offset (= 16 + 64*12 = 784)
#define RF_NUM_PKTS      64u
#define RF_PKT_SIZE      3072u
#define RF_HDR_BYTES     16u
#define RF_TABLE_BYTES   (RF_NUM_PKTS * 12u)
#define RF_DATA_OFFSET   (RF_HDR_BYTES + RF_TABLE_BYTES)    // = 784
#define RF_DATA_BYTES    (RF_NUM_PKTS * RF_PKT_SIZE)        // = 196608
#define RF_TOTAL_BYTES   (RF_DATA_OFFSET + RF_DATA_BYTES)   // = 197392

static const UCHAR g_InitBlock0100[120] = { 0 };
static const UCHAR g_InitBlock0000[120] = {
    0x00,0x00,0x78,0x80,0x00,0x00,0x00,0x03,0x00,0x00,0x46,0x46,0x00,0x00,0x00,0x02,
    0x00,0x00,0x00,0x00,0x46,0x46,0x46,0x46,0x10,0x00,0x15,0x0e,0x46,0x46,0x46,0x46,
    0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,
    0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,
    0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,
    0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,
    0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,
    0x46,0x46,0x46,0x46,0x46,0x46,0x46,0x46,
};

// -------------------------------------------------------------------------
// Per-ReadFile passthrough context.
//
// One of these is attached to each IRP_MJ_READ / READ_ISO_PIPE WDFREQUEST
// via WdfObjectAllocateContext.  It holds the URB, the partial MDL into the
// DLL's buffer, and the bookkeeping the WDF completion callback needs to
// write IsoPacket results back into the DLL's table.
//
// The URB itself is parented to the WDFREQUEST so WDF auto-frees it when
// the request is completed.  WDF also handles IRP allocation/cancellation
// transparently via WdfUsbTargetPipeFormatRequestForUrb + WdfRequestSend.
// -------------------------------------------------------------------------
typedef struct _NET1120_RF_CONTEXT {
    struct _NET1120_DEVICE_EXT* Ext;
    PURB        Urb;
    WDFMEMORY   UrbMem;          // tied to Request lifetime by parent attribute
    PMDL        PartialMdl;      // partial MDL covering data area; we IoFreeMdl in completion
    PUCHAR      MappedBuffer;    // kernel VA of the full DLL buffer (header+table+data)
    ULONG       NumPackets;
    ULONG       BaseOffset;      // table[0].data_offset (typically 784)
    LONG        Completed;       // V161: 0 = not yet completed, 1 = completion in progress
    //       Used as InterlockedExchange gate to prevent
    //       double-completion if WDF somehow re-enters us.
} NET1120_RF_CONTEXT, * PNET1120_RF_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NET1120_RF_CONTEXT, GetRfContext)

// -------------------------------------------------------------------------
// Frame descriptor (mapped to user space) — kept for compatibility with the
// SET_DEVICE_PARAMETERS dwOptions=0x10000 path.  Even though the DLL does
// not use this on the steady-state data path (ReadFile is the data path),
// some IOCTL handlers in the DLL do touch UNBIND_PIPE return values; we
// keep the descriptor allocation as a no-op-equivalent shape so behaviour
// matches v158 for those non-data paths.
// -------------------------------------------------------------------------
typedef struct _NET1120_FRAME_DESC {
    ULONG  Reserved0;
    ULONG  FormatType;       // 1=NTSC
    ULONG  Reserved1;
    USHORT FrameWidth;
    USHORT Reserved2;
    ULONG  Reserved3;
    ULONG  FrameStride;
    ULONG  Reserved4;
    ULONG  Reserved5;
    ULONG  FrameHeight;
    ULONG  PixelDataPtr;
} NET1120_FRAME_DESC;

// -------------------------------------------------------------------------
// Device extension
// -------------------------------------------------------------------------
typedef struct _NET1120_DEVICE_EXT {
    WDFDEVICE         WdfDevice;
    WDFUSBDEVICE      UsbDevice;
    USBD_HANDLE       UsbdHandle;
    WDFUSBINTERFACE   VideoInterface;
    WDFUSBPIPE        IsochPipe;
    USBD_PIPE_HANDLE  IsochPipeHandleWdm;   // cached at StartStreaming
    PDEVICE_OBJECT    IsochTargetDevObj;     // cached at StartStreaming
    USB_DEVICE_DESCRIPTOR DeviceDesc;

    BOOLEAN           Acquired;
    BOOLEAN           Configured;
    UCHAR             CurrentConfig;
    UCHAR             CurrentAltSetting;
    ULONG             DeviceOptions;
    ULONG             RequestTimeout;
    UCHAR             BoundEndpoint;

    BOOLEAN           Streaming;
    KSPIN_LOCK        StreamLock;

    // Active passthrough request count.  StopStreaming polls for this to
    // drop to zero after aborting the pipe before tearing down.
    LONG              ActivePassthroughRequests;

} NET1120_DEVICE_EXT, * PNET1120_DEVICE_EXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NET1120_DEVICE_EXT, GetDevExt)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD             EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE       EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE       EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY               EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT                EvtD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL    EvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_READ              EvtIoRead;
EVT_WDF_REQUEST_COMPLETION_ROUTINE    PassthroughUrbComplete;
EVT_WDF_DEVICE_FILE_CREATE            EvtDeviceFileCreate;
EVT_WDF_FILE_CLOSE                    EvtDeviceFileClose;

NTSTATUS WriteReg(PNET1120_DEVICE_EXT d, USHORT reg, UCHAR val);
NTSTATUS ReadReg(PNET1120_DEVICE_EXT d, USHORT reg, PUCHAR val);
NTSTATUS InitDevice(PNET1120_DEVICE_EXT d);
NTSTATUS StartStreaming(PNET1120_DEVICE_EXT d);
NTSTATUS StopStreaming(PNET1120_DEVICE_EXT d);
NTSTATUS SubmitPassthroughRead(PNET1120_DEVICE_EXT d, WDFREQUEST Request);