/*
 * ioctl.c - Control device: userspace ↔ kernel communication
 * Userspace opens \\.\mt7921mon and sends IOCTLs
 */
#include "filter.h"

static PDEVICE_OBJECT g_ctrl_dev = NULL;

/* ── IRP_MJ_CREATE / IRP_MJ_CLOSE ───────────────────────────── */
static NTSTATUS IrpCreateClose(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ── Get first active filter context ─────────────────────────── */
static PFILTER_CONTEXT GetFirstContext(void)
{
    PFILTER_CONTEXT ctx = NULL;
    NdisAcquireSpinLock(&g.filter_list_lock);
    if (!IsListEmpty(&g.filter_list)) {
        ctx = CONTAINING_RECORD(g.filter_list.Flink,
                                FILTER_CONTEXT, list_entry);
    }
    NdisReleaseSpinLock(&g.filter_list_lock);
    return ctx;
}

/* ── IRP_MJ_DEVICE_CONTROL ───────────────────────────────────── */
NTSTATUS IoctlDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION  stack  = IoGetCurrentIrpStackLocation(irp);
    ULONG               code   = stack->Parameters.DeviceIoControl.IoControlCode;
    PVOID               in_buf = irp->AssociatedIrp.SystemBuffer;
    PVOID               out_buf;
    ULONG               in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG               out_len= stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS            status = STATUS_SUCCESS;
    ULONG_PTR           info   = 0;
    PFILTER_CONTEXT     ctx;

    UNREFERENCED_PARAMETER(dev);

    ctx = GetFirstContext();
    if (!ctx && code != IOCTL_GET_STATUS) {
        status = STATUS_DEVICE_NOT_READY;
        goto done;
    }

    switch (code) {

    /* ── Enable Monitor Mode ──────────────────────────────────── */
    case IOCTL_ENABLE_MONITOR: {
        NDIS_STATUS ns = MonitorEnable(ctx);
        status = (ns == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS
                                              : STATUS_UNSUCCESSFUL;
        break;
    }

    /* ── Disable Monitor Mode ─────────────────────────────────── */
    case IOCTL_DISABLE_MONITOR: {
        NDIS_STATUS ns = MonitorDisable(ctx);
        status = (ns == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS
                                              : STATUS_UNSUCCESSFUL;
        break;
    }

    /* ── Set Channel ──────────────────────────────────────────── */
    case IOCTL_SET_CHANNEL: {
        PMONITOR_CHANNEL_REQ req;
        NDIS_STATUS ns;
        if (in_len < sizeof(MONITOR_CHANNEL_REQ)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        req = (PMONITOR_CHANNEL_REQ)in_buf;
        ns = MonitorSetChannel(ctx, req->channel, req->bandwidth, req->sco);
        status = (ns == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        break;
    }

    case IOCTL_GET_STATUS: {
        PMONITOR_STATUS s;
        if (out_len < sizeof(MONITOR_STATUS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        out_buf = irp->AssociatedIrp.SystemBuffer;
        s = (PMONITOR_STATUS)out_buf;
        RtlZeroMemory(s, sizeof(MONITOR_STATUS));
        /* Driver-wide diagnostics are always available, even with no adapter */
        s->diag_register_status = g.diag_register_status;
        s->diag_ctrldev_status  = g.diag_ctrldev_status;
        s->diag_attach_count    = (UINT32)g.diag_attach_count;
        if (ctx) {
            s->active            = ctx->monitor_active ? 1 : 0;
            s->channel           = ctx->current_channel;
            s->bandwidth         = ctx->current_bw;
            s->packets_captured  = ctx->packets_captured;
            s->packets_dropped   = ctx->packets_dropped;
            s->diag_media_type   = ctx->diag_media_type;
            s->diag_last_ndis_mode   = ctx->diag_last_ndis_mode;
            s->diag_last_mcu_sniffer = ctx->diag_last_mcu_sniffer;
            s->diag_last_pkt_filter  = ctx->diag_last_pkt_filter;
            s->diag_last_channel     = ctx->diag_last_channel;
        }
        info   = sizeof(MONITOR_STATUS);
        break;
    }

    /* ── Read Captured Packet ─────────────────────────────────── */
    case IOCTL_READ_PACKET: {
        PUCHAR out_va;
        LARGE_INTEGER timeout;
        UINT32 read;
        if (!ctx) { status = STATUS_DEVICE_NOT_READY; break; }

        out_va = (PUCHAR)MmGetSystemAddressForMdlSafe(
                     irp->MdlAddress, NormalPagePriority);
        if (!out_va) { status = STATUS_INSUFFICIENT_RESOURCES; break; }

        if (ctx->ring.head == ctx->ring.tail) {
            timeout.QuadPart = -10 * 1000 * 1000 * 1;
            KeWaitForSingleObject(&ctx->ring.data_event, UserRequest,
                                  KernelMode, TRUE, &timeout);
        }

        read = RingRead(&ctx->ring, out_va, out_len);
        info = read;
        if (read == 0) status = STATUS_TIMEOUT;
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

done:
    irp->IoStatus.Status      = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

/* ── CreateControlDevice ─────────────────────────────────────── */
NDIS_STATUS CreateControlDevice(PDRIVER_OBJECT drv)
{
    NTSTATUS       status;
    UNICODE_STRING dev_name, link_name;

    RtlInitUnicodeString(&dev_name,  DEVICE_NAME);
    RtlInitUnicodeString(&link_name, DEVICE_LINK);

    status = IoCreateDevice(drv, 0, &dev_name,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE, &g_ctrl_dev);
    if (!NT_SUCCESS(status)) return NDIS_STATUS_FAILURE;

    g_ctrl_dev->Flags &= ~DO_DEVICE_INITIALIZING;
    g_ctrl_dev->Flags |=  DO_DIRECT_IO;

    drv->MajorFunction[IRP_MJ_CREATE]         = IrpCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = IrpCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = IoctlDispatch;

    status = IoCreateSymbolicLink(&link_name, &dev_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_ctrl_dev);
        g_ctrl_dev = NULL;
        return NDIS_STATUS_FAILURE;
    }

    return NDIS_STATUS_SUCCESS;
}

/* ── DeleteControlDevice ─────────────────────────────────────── */
VOID DeleteControlDevice(VOID)
{
    UNICODE_STRING link_name;
    RtlInitUnicodeString(&link_name, DEVICE_LINK);
    IoDeleteSymbolicLink(&link_name);
    if (g_ctrl_dev) {
        IoDeleteDevice(g_ctrl_dev);
        g_ctrl_dev = NULL;
    }
}
