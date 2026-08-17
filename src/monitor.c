/*
 * monitor.c - Monitor mode control via NDIS OID + MT7921 firmware commands
 *
 * Strategy:
 *   1. Try NDIS Native WiFi OID (OID_DOT11_CURRENT_OPERATION_MODE)
 *      → Request NetworkMonitor mode from the existing MediaTek driver
 *   2. If rejected, send MCU_UNI_CMD_SNIFFER directly via OID_DOT11_NIC_SPECIFIC_EXTENSION
 *      (raw firmware command passthrough, supported by some MediaTek drivers)
 */
#include "filter.h"
#include <windot11.h>   /* DOT11_OPERATION_MODE_* */

/* ── Internal: send OID to miniport and wait ─────────────────── */
static NDIS_STATUS SendOidSync(PFILTER_CONTEXT ctx,
                                NDIS_REQUEST_TYPE type,
                                NDIS_OID oid,
                                PVOID info,
                                UINT32 info_len,
                                UINT32 *bytes_used)
{
    NDIS_STATUS status;
    LARGE_INTEGER timeout;
    timeout.QuadPart = -10 * 1000 * 1000 * 5; /* 5 seconds */

    RtlZeroMemory(&ctx->oid_request, sizeof(ctx->oid_request));
    ctx->oid_request.Header.Type     = NDIS_OBJECT_TYPE_OID_REQUEST;
    ctx->oid_request.Header.Revision = NDIS_OID_REQUEST_REVISION_1;
    ctx->oid_request.Header.Size     = NDIS_SIZEOF_OID_REQUEST_REVISION_1;
    ctx->oid_request.RequestType     = type;

    if (type == NdisRequestSetInformation) {
        ctx->oid_request.DATA.SET_INFORMATION.Oid              = oid;
        ctx->oid_request.DATA.SET_INFORMATION.InformationBuffer    = info;
        ctx->oid_request.DATA.SET_INFORMATION.InformationBufferLength = info_len;
    } else {
        ctx->oid_request.DATA.QUERY_INFORMATION.Oid            = oid;
        ctx->oid_request.DATA.QUERY_INFORMATION.InformationBuffer  = info;
        ctx->oid_request.DATA.QUERY_INFORMATION.InformationBufferLength = info_len;
    }

    KeClearEvent(&ctx->oid_complete_event);
    status = NdisFOidRequest(ctx->filter_handle, &ctx->oid_request);

    if (status == NDIS_STATUS_PENDING) {
        KeWaitForSingleObject(&ctx->oid_complete_event, Executive,
                              KernelMode, FALSE, &timeout);
        status = ctx->oid_status;
    }

    if (bytes_used && type == NdisRequestSetInformation)
        *bytes_used = ctx->oid_request.DATA.SET_INFORMATION.BytesRead;

    return status;
}

/* ── Step 1: Ask driver to switch to Network Monitor mode ────── */
static NDIS_STATUS TryNdisMonitorMode(PFILTER_CONTEXT ctx, BOOLEAN enable)
{
    DOT11_CURRENT_OPERATION_MODE mode;
    NDIS_STATUS status;

    RtlZeroMemory(&mode, sizeof(mode));
    mode.uCurrentOpMode = enable ? DOT11_OPERATION_MODE_NETWORK_MONITOR
                                 : DOT11_OPERATION_MODE_EXTENSIBLE_STATION;

    status = SendOidSync(ctx, NdisRequestSetInformation,
                         OID_DOT11_CURRENT_OPERATION_MODE,
                         &mode, sizeof(mode), NULL);
    ctx->diag_last_ndis_mode = status;
    MON_LOG("OID_DOT11_CURRENT_OPERATION_MODE(monitor=%d) = 0x%08X\n",
            enable, status);
    return status;
}

/* ── Step 2: Set packet filter to capture raw 802.11 frames ──── */
static NDIS_STATUS SetRawPacketFilter(PFILTER_CONTEXT ctx, BOOLEAN enable)
{
    ULONG filter_flags;

    if (enable) {
        filter_flags = NDIS_PACKET_TYPE_PROMISCUOUS
                     | NDIS_PACKET_TYPE_802_11_RAW_DATA
                     | NDIS_PACKET_TYPE_802_11_RAW_MGMT;
    } else {
        filter_flags = NDIS_PACKET_TYPE_DIRECTED
                     | NDIS_PACKET_TYPE_MULTICAST
                     | NDIS_PACKET_TYPE_BROADCAST;
    }

    {
        NDIS_STATUS s = SendOidSync(ctx, NdisRequestSetInformation,
                                    OID_GEN_CURRENT_PACKET_FILTER,
                                    &filter_flags, sizeof(filter_flags), NULL);
        ctx->diag_last_pkt_filter = s;
        MON_LOG("OID_GEN_CURRENT_PACKET_FILTER(0x%08X) = 0x%08X\n",
                filter_flags, s);
        return s;
    }
}

/* ── Step 3 (fallback): send MCU sniffer command via NIC extension OID ── */
NDIS_STATUS MonitorSendMcuSniffer(PFILTER_CONTEXT ctx, BOOLEAN enable)
{
    MCU_SNIFFER_ENABLE_MSG msg;
    NDIS_STATUS status;

    RtlZeroMemory(&msg, sizeof(msg));

    /* Build MCU header */
    msg.hdr.cmd = MCU_UNI_CMD_SNIFFER;
    msg.hdr.seq = ++ctx->seq;
    msg.hdr.len = (UINT16)sizeof(SNIFFER_ENABLE_TLV);

    /* Build TLV */
    msg.tlv.hdr.tag = SNIFFER_TLV_ENABLE;
    msg.tlv.hdr.len = (UINT16)sizeof(SNIFFER_ENABLE_TLV);
    msg.tlv.enable  = enable ? 1 : 0;

    /*
     * OID_DOT11_NIC_SPECIFIC_EXTENSION passes arbitrary buffers to the
     * miniport — some MediaTek drivers forward these to the firmware.
     * If this also fails, we fall back to IOCTL-based PCIe register access.
     */
    status = SendOidSync(ctx, NdisRequestSetInformation,
                         OID_DOT11_NIC_SPECIFIC_EXTENSION,
                         &msg, sizeof(msg), NULL);
    ctx->diag_last_mcu_sniffer = status;
    MON_LOG("MCU_SNIFFER(enable=%d) via NIC_SPECIFIC_EXTENSION = 0x%08X\n",
            enable, status);
    return status;
}

/* ── Send MCU channel config ─────────────────────────────────── */
NDIS_STATUS MonitorSendMcuChannel(PFILTER_CONTEXT ctx,
                                   UINT8 ch, UINT8 band, UINT8 bw, UINT8 sco)
{
    MCU_SNIFFER_CONFIG_MSG msg;

    RtlZeroMemory(&msg, sizeof(msg));

    msg.hdr.cmd = MCU_UNI_CMD_SNIFFER;
    msg.hdr.seq = ++ctx->seq;
    msg.hdr.len = (UINT16)sizeof(SNIFFER_CONFIG_TLV);

    msg.tlv.hdr.tag   = SNIFFER_TLV_CONFIG;
    msg.tlv.hdr.len   = (UINT16)sizeof(SNIFFER_CONFIG_TLV);
    msg.tlv.ch_band   = band;
    msg.tlv.bw        = bw;
    msg.tlv.control_ch = ch;
    msg.tlv.sco       = sco;
    msg.tlv.center_ch = ch;     /* simplified — correct for 20MHz */
    msg.tlv.drop_err  = 0;

    return SendOidSync(ctx, NdisRequestSetInformation,
                       OID_DOT11_NIC_SPECIFIC_EXTENSION,
                       &msg, sizeof(msg), NULL);
}

/* ── MonitorEnable: full enable sequence ─────────────────────── */
NDIS_STATUS MonitorEnable(PFILTER_CONTEXT ctx)
{
    NDIS_STATUS status;

    if (ctx->monitor_active) return NDIS_STATUS_SUCCESS;

    MON_LOG("MonitorEnable: begin (IfIndex=%u)\n", ctx->if_index);

    /* Try NDIS Native WiFi operation mode switch */
    status = TryNdisMonitorMode(ctx, TRUE);
    if (status != NDIS_STATUS_SUCCESS) {
        /* Driver rejected the NDIS mode switch — try MCU command directly */
        MON_LOG("MonitorEnable: NDIS mode rejected, trying raw MCU command\n");
        status = MonitorSendMcuSniffer(ctx, TRUE);
        if (status != NDIS_STATUS_SUCCESS) {
            MON_LOG("MonitorEnable: FAILED - firmware rejected both paths\n");
            return status;
        }
    }

    /* Enable promiscuous / raw 802.11 packet filter */
    SetRawPacketFilter(ctx, TRUE);   /* best-effort, ignore failure */

    ctx->monitor_active = TRUE;
    MON_LOG("MonitorEnable: SUCCESS - monitor mode active\n");
    return NDIS_STATUS_SUCCESS;
}

/* ── MonitorDisable ──────────────────────────────────────────── */
NDIS_STATUS MonitorDisable(PFILTER_CONTEXT ctx)
{
    if (!ctx->monitor_active) return NDIS_STATUS_SUCCESS;

    MonitorSendMcuSniffer(ctx, FALSE);
    TryNdisMonitorMode(ctx, FALSE);
    SetRawPacketFilter(ctx, FALSE);

    ctx->monitor_active = FALSE;
    return NDIS_STATUS_SUCCESS;
}

/* ── MonitorSetChannel ───────────────────────────────────────── */
NDIS_STATUS MonitorSetChannel(PFILTER_CONTEXT ctx,
                               UINT8 channel, UINT8 bw, UINT8 sco)
{
    UINT8 band = ChannelToBand(channel);
    NDIS_STATUS status;

    /* Try NDIS channel switch via OID_DOT11_CURRENT_CHANNEL (ULONG) */
    {
        ULONG ch = channel;
        status = SendOidSync(ctx, NdisRequestSetInformation,
                             OID_DOT11_CURRENT_CHANNEL,
                             &ch, sizeof(ch), NULL);
        ctx->diag_last_channel = status;
        MON_LOG("OID_DOT11_CURRENT_CHANNEL(ch=%u) = 0x%08X\n", channel, status);
    }

    /* Always also send MCU command */
    MonitorSendMcuChannel(ctx, channel, band, bw, sco);

    ctx->current_channel = channel;
    ctx->current_bw      = bw;
    return NDIS_STATUS_SUCCESS;
}
