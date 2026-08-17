/*
 * filter.h - MT7921 Monitor Mode NDIS Lightweight Filter Driver
 */
#pragma once

/* Tell ndis.h which NDIS version to target.
 * NDIS680 causes ndis.h to set NDIS_FILTER_MAJOR/MINOR_VERSION itself.
 * Never define NDIS_FILTER_MAJOR/MINOR_VERSION directly — ndis.h forbids it. */
#define NDIS680              1
#define NDIS_SUPPORT_NDIS680 1

#include <ndis.h>
#include <wdm.h>
#include "mt7921_fw.h"

/* ── Diagnostic logging ──────────────────────────────────────────
 * MON_LOG prints via DbgPrintEx at ERROR level so it is visible by
 * default in DebugView (Capture Kernel) and WinDbg without changing
 * the debug print filter mask. First variadic arg must be a literal. */
#define MON_LOG(...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[mt7921mon] " __VA_ARGS__)

/* ── Identity ────────────────────────────────────────────────── */
#define FILTER_FRIENDLY_NAME    L"MT7921 Monitor Mode Filter"
#define FILTER_UNIQUE_NAME      L"{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}"
#define FILTER_SERVICE_NAME     L"mt7921mon"
#define DEVICE_NAME             L"\\Device\\mt7921mon"
#define DEVICE_LINK             L"\\DosDevices\\mt7921mon"
#define POOL_TAG                'nomf'

/* ── IOCTLs ──────────────────────────────────────────────────── */
#define IOCTL_BASE              0x8000
#define IOCTL_ENABLE_MONITOR  CTL_CODE(FILE_DEVICE_UNKNOWN,IOCTL_BASE+1,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define IOCTL_DISABLE_MONITOR CTL_CODE(FILE_DEVICE_UNKNOWN,IOCTL_BASE+2,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define IOCTL_SET_CHANNEL     CTL_CODE(FILE_DEVICE_UNKNOWN,IOCTL_BASE+3,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define IOCTL_GET_STATUS      CTL_CODE(FILE_DEVICE_UNKNOWN,IOCTL_BASE+4,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define IOCTL_READ_PACKET     CTL_CODE(FILE_DEVICE_UNKNOWN,IOCTL_BASE+5,METHOD_OUT_DIRECT,FILE_ANY_ACCESS)

/* ── IOCTL structures ────────────────────────────────────────── */
#pragma pack(push,1)
typedef struct _MONITOR_CHANNEL_REQ {
    UINT8  channel;
    UINT8  bandwidth;
    UINT8  sco;
    UINT8  reserved;
} MONITOR_CHANNEL_REQ, *PMONITOR_CHANNEL_REQ;

typedef struct _MONITOR_STATUS {
    UINT8  active;
    UINT8  channel;
    UINT8  bandwidth;
    UINT8  reserved;
    UINT32 packets_captured;
    UINT32 packets_dropped;
    /* ── Diagnostics (appended; old offsets unchanged) ── */
    INT32  diag_register_status;   /* NdisFRegisterFilterDriver result   */
    INT32  diag_ctrldev_status;    /* CreateControlDevice result         */
    UINT32 diag_attach_count;      /* # adapters our filter attached to   */
    UINT32 diag_media_type;        /* MiniportMediaType seen at attach    */
    INT32  diag_last_ndis_mode;    /* OID_DOT11_CURRENT_OPERATION_MODE    */
    INT32  diag_last_mcu_sniffer;  /* MCU sniffer via NIC_SPECIFIC_EXT    */
    INT32  diag_last_pkt_filter;   /* OID_GEN_CURRENT_PACKET_FILTER       */
    INT32  diag_last_channel;      /* OID_DOT11_CURRENT_CHANNEL           */
} MONITOR_STATUS, *PMONITOR_STATUS;

typedef struct _CAPTURE_PKT_HDR {
    UINT32 magic;
    UINT16 caplen;
    UINT16 origlen;
    UINT64 timestamp_us;
    UINT8  channel;
    UINT8  rssi;
    UINT8  data_rate;
    UINT8  flags;
} CAPTURE_PKT_HDR, *PCAPTURE_PKT_HDR;
#pragma pack(pop)

#define CAPTURE_PKT_MAGIC   0xA1B2C3D4

/* ── Ring buffer ─────────────────────────────────────────────── */
#define RING_BUF_SIZE   (2 * 1024 * 1024)
#define MAX_PKT_SIZE    2346

typedef struct _RING_BUFFER {
    PUCHAR          buf;
    UINT32          size;
    volatile LONG   head;
    volatile LONG   tail;
    KEVENT          data_event;
    KSPIN_LOCK      lock;
} RING_BUFFER, *PRING_BUFFER;

/* ── Filter context ──────────────────────────────────────────── */
typedef struct _FILTER_CONTEXT {
    LIST_ENTRY          list_entry;
    NDIS_HANDLE         filter_handle;
    NET_IFINDEX         if_index;
    WCHAR               guid_name[128];
    BOOLEAN             monitor_active;
    UINT8               current_channel;
    UINT8               current_bw;
    UINT32              packets_captured;
    UINT32              packets_dropped;
    RING_BUFFER         ring;
    NDIS_OID_REQUEST    oid_request;
    KEVENT              oid_complete_event;
    NDIS_STATUS         oid_status;
    NDIS_SPIN_LOCK      lock;
    UINT8               seq;
    /* ── Per-adapter diagnostics ── */
    ULONG               diag_media_type;
    NDIS_STATUS         diag_last_ndis_mode;
    NDIS_STATUS         diag_last_mcu_sniffer;
    NDIS_STATUS         diag_last_pkt_filter;
    NDIS_STATUS         diag_last_channel;
} FILTER_CONTEXT, *PFILTER_CONTEXT;

/* ── Global state ────────────────────────────────────────────── */
typedef struct _DRIVER_GLOBALS {
    NDIS_HANDLE         filter_driver_handle;
    LIST_ENTRY          filter_list;
    NDIS_SPIN_LOCK      filter_list_lock;
    /* ── Driver-wide diagnostics ── */
    NDIS_STATUS         diag_register_status;
    NDIS_STATUS         diag_ctrldev_status;
    volatile LONG       diag_attach_count;
} DRIVER_GLOBALS;

extern DRIVER_GLOBALS g;

/* ── Prototypes ──────────────────────────────────────────────── */
/* filter.c */
NDIS_STATUS FilterAttach(NDIS_HANDLE, NDIS_HANDLE, PNDIS_FILTER_ATTACH_PARAMETERS);
VOID        FilterDetach(NDIS_HANDLE);
NDIS_STATUS FilterRestart(NDIS_HANDLE, PNDIS_FILTER_RESTART_PARAMETERS);
NDIS_STATUS FilterPause(NDIS_HANDLE, PNDIS_FILTER_PAUSE_PARAMETERS);
VOID        FilterReceiveNetBufferLists(NDIS_HANDLE, PNET_BUFFER_LIST, NDIS_PORT_NUMBER, ULONG, ULONG);
VOID        FilterSendNetBufferLists(NDIS_HANDLE, PNET_BUFFER_LIST, NDIS_PORT_NUMBER, ULONG);
VOID        FilterSendNetBufferListsComplete(NDIS_HANDLE, PNET_BUFFER_LIST, ULONG);
VOID        FilterReturnNetBufferLists(NDIS_HANDLE, PNET_BUFFER_LIST, ULONG);
VOID        FilterCancelSendNetBufferLists(NDIS_HANDLE, PVOID);
NDIS_STATUS FilterOidRequest(NDIS_HANDLE, PNDIS_OID_REQUEST);
VOID        FilterOidRequestComplete(NDIS_HANDLE, PNDIS_OID_REQUEST, NDIS_STATUS);
PFILTER_CONTEXT FindContextByIfIndex(NET_IFINDEX idx);

/* monitor.c */
NDIS_STATUS MonitorEnable(PFILTER_CONTEXT ctx);
NDIS_STATUS MonitorDisable(PFILTER_CONTEXT ctx);
NDIS_STATUS MonitorSetChannel(PFILTER_CONTEXT ctx, UINT8 channel, UINT8 bw, UINT8 sco);
NDIS_STATUS MonitorSendMcuSniffer(PFILTER_CONTEXT ctx, BOOLEAN enable);
NDIS_STATUS MonitorSendMcuChannel(PFILTER_CONTEXT ctx, UINT8 ch, UINT8 band, UINT8 bw, UINT8 sco);

/* packet.c */
VOID    RingInit(PRING_BUFFER ring);
VOID    RingFree(PRING_BUFFER ring);
BOOLEAN RingWrite(PRING_BUFFER ring, PUCHAR data, UINT32 len);
UINT32  RingRead(PRING_BUFFER ring, PUCHAR out, UINT32 max_len);
VOID    CaptureFrame(PFILTER_CONTEXT ctx, PNET_BUFFER nb, UINT8 channel, INT8 rssi);

/* ioctl.c */
NDIS_STATUS CreateControlDevice(PDRIVER_OBJECT drv);
VOID        DeleteControlDevice(VOID);
NTSTATUS    IoctlDispatch(PDEVICE_OBJECT dev, PIRP irp);
