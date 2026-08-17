/*
 * filter.c - NDIS LWF main entry
 */
#pragma warning(disable: 4996)  /* ExAllocatePoolWithTag deprecated */
#include "filter.h"

DRIVER_GLOBALS g;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     FilterUnload;

/* Write a DWORD diagnostic value to HKLM\SOFTWARE\mt7921mon so we can
 * see how far DriverEntry got at boot (where DbgPrint is not visible).
 * The key is pre-created from user space; if absent this is a no-op. */
static VOID DiagWrite(PCWSTR name, ULONG value)
{
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE,
                          L"\\Registry\\Machine\\SOFTWARE\\mt7921mon",
                          (PWSTR)name, REG_DWORD, &value, sizeof(value));
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NDIS_STATUS status;
    NDIS_FILTER_DRIVER_CHARACTERISTICS chars;

    UNREFERENCED_PARAMETER(RegistryPath);

    DiagWrite(L"Stage", 1);          /* DriverEntry entered */
    DiagWrite(L"RegisterStatus", 0xFFFFFFFF);
    DiagWrite(L"CtrlDevStatus",  0xFFFFFFFF);
    DiagWrite(L"AttachCount", 0);
    DiagWrite(L"AttachAttempts", 0);
    DiagWrite(L"LastAttachMediaType", 0xFFFFFFFF);

    RtlZeroMemory(&g, sizeof(g));
    InitializeListHead(&g.filter_list);
    NdisAllocateSpinLock(&g.filter_list_lock);

    RtlZeroMemory(&chars, sizeof(chars));
    chars.Header.Type     = NDIS_OBJECT_TYPE_FILTER_DRIVER_CHARACTERISTICS;
    /* REVISION_2 (NDIS 6.1+): does NOT require the 6.80 SynchronousOid
     * handlers. Declaring NDIS 6.30 keeps us maximally compatible; we use
     * no 6.80-specific features. REVISION_3 was rejected as
     * NDIS_STATUS_BAD_CHARACTERISTICS because its sync-OID handlers are
     * mandatory but we pass through. */
    chars.Header.Revision = NDIS_FILTER_CHARACTERISTICS_REVISION_2;
    chars.Header.Size     = NDIS_SIZEOF_FILTER_DRIVER_CHARACTERISTICS_REVISION_2;

    chars.MajorNdisVersion   = 6;
    chars.MinorNdisVersion   = 30;   /* NDIS 6.30 */
    chars.MajorDriverVersion = 1;
    chars.MinorDriverVersion = 0;
    chars.Flags              = 0;

    RtlInitUnicodeString(&chars.FriendlyName,  FILTER_FRIENDLY_NAME);
    RtlInitUnicodeString(&chars.UniqueName,    FILTER_UNIQUE_NAME);
    RtlInitUnicodeString(&chars.ServiceName,   FILTER_SERVICE_NAME);

    chars.AttachHandler                          = FilterAttach;
    chars.DetachHandler                          = FilterDetach;
    chars.RestartHandler                         = FilterRestart;
    chars.PauseHandler                           = FilterPause;
    /* OID request + complete MUST be provided as a pair (NDIS requirement).
     * We originate our own OID commands, so we need the complete handler,
     * hence we must also provide the request (pass-through) handler. */
    chars.OidRequestHandler                      = FilterOidRequest;
    chars.OidRequestCompleteHandler              = FilterOidRequestComplete;
    chars.ReceiveNetBufferListsHandler           = FilterReceiveNetBufferLists;
    chars.ReturnNetBufferListsHandler            = FilterReturnNetBufferLists;
    chars.SendNetBufferListsHandler              = FilterSendNetBufferLists;
    chars.SendNetBufferListsCompleteHandler      = FilterSendNetBufferListsComplete;
    chars.CancelSendNetBufferListsHandler        = FilterCancelSendNetBufferLists;
    /* REVISION_2 optional direct-OID handlers — not used (pass-through) */
    chars.DirectOidRequestHandler                = NULL;
    chars.DirectOidRequestCompleteHandler        = NULL;
    chars.CancelDirectOidRequestHandler          = NULL;

    MON_LOG("DriverEntry: registering filter (NDIS %u.%u)\n",
            NDIS_FILTER_MAJOR_VERSION, NDIS_FILTER_MINOR_VERSION);

    status = NdisFRegisterFilterDriver(DriverObject,
                                       (NDIS_HANDLE)DriverObject,
                                       &chars,
                                       &g.filter_driver_handle);
    g.diag_register_status = status;
    DiagWrite(L"RegisterStatus", (ULONG)status);
    if (status != NDIS_STATUS_SUCCESS) {
        MON_LOG("DriverEntry: NdisFRegisterFilterDriver FAILED = 0x%08X\n", status);
        DiagWrite(L"Stage", 99);
        return status;
    }
    MON_LOG("DriverEntry: filter registered OK\n");
    DiagWrite(L"Stage", 2);

    status = CreateControlDevice(DriverObject);
    g.diag_ctrldev_status = status;
    DiagWrite(L"CtrlDevStatus", (ULONG)status);
    if (status != NDIS_STATUS_SUCCESS) {
        MON_LOG("DriverEntry: CreateControlDevice FAILED = 0x%08X\n", status);
        NdisFDeregisterFilterDriver(g.filter_driver_handle);
        DiagWrite(L"Stage", 98);
        return status;
    }
    MON_LOG("DriverEntry: control device \\\\.\\mt7921mon created OK\n");

    DriverObject->DriverUnload = FilterUnload;
    MON_LOG("DriverEntry: SUCCESS, driver loaded\n");
    DiagWrite(L"Stage", 3);          /* full success */
    return STATUS_SUCCESS;
}

VOID FilterUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DeleteControlDevice();
    NdisFDeregisterFilterDriver(g.filter_driver_handle);
    NdisFreeSpinLock(&g.filter_list_lock);
}

NDIS_STATUS FilterAttach(NDIS_HANDLE NdisFilterHandle,
                          NDIS_HANDLE FilterDriverContext,
                          PNDIS_FILTER_ATTACH_PARAMETERS AttachParameters)
{
    PFILTER_CONTEXT ctx;
    NDIS_STATUS     status;
    NDIS_FILTER_ATTRIBUTES attrs;

    UNREFERENCED_PARAMETER(FilterDriverContext);

    /* Record every attach attempt + the media type NDIS presents, so we
     * can diagnose from user space without a kernel debugger. */
    {
        static volatile LONG s_attempts = 0;
        DiagWrite(L"AttachAttempts", (ULONG)InterlockedIncrement(&s_attempts));
        DiagWrite(L"LastAttachMediaType", (ULONG)AttachParameters->MiniportMediaType);
        DiagWrite(L"LastAttachIfIndex", (ULONG)AttachParameters->IfIndex);
        /* If any adapter presents as raw Native 802.11, record it —
         * that is the edge where monitor-mode injection can work. */
        if (AttachParameters->MiniportMediaType == NdisMediumNative802_11)
            DiagWrite(L"NativeWifiIfIndex", (ULONG)AttachParameters->IfIndex);
    }

    MON_LOG("FilterAttach: IfIndex=%u MediaType=%d (Native802_11=%d)\n",
            AttachParameters->IfIndex,
            AttachParameters->MiniportMediaType,
            NdisMediumNative802_11);

    /* Accept Native 802.11 (raw) and 802.3-emulated WLAN edges. Reject
     * anything else (real Ethernet is excluded via INF FilterMediaTypes). */
    if (AttachParameters->MiniportMediaType != NdisMediumNative802_11 &&
        AttachParameters->MiniportMediaType != NdisMedium802_3) {
        MON_LOG("FilterAttach: skipping media type %d\n",
                AttachParameters->MiniportMediaType);
        return NDIS_STATUS_FAILURE;
    }

    ctx = (PFILTER_CONTEXT)ExAllocatePoolWithTag(NonPagedPool,
                                                  sizeof(FILTER_CONTEXT),
                                                  POOL_TAG);
    if (!ctx) return NDIS_STATUS_RESOURCES;
    RtlZeroMemory(ctx, sizeof(FILTER_CONTEXT));

    ctx->filter_handle   = NdisFilterHandle;
    ctx->if_index        = AttachParameters->IfIndex;
    ctx->diag_media_type = AttachParameters->MiniportMediaType;

    /* Save GUID name for identification */
    if (AttachParameters->FilterModuleGuidName &&
        AttachParameters->FilterModuleGuidName->Length <
        sizeof(ctx->guid_name) - sizeof(WCHAR)) {
        RtlCopyMemory(ctx->guid_name,
                      AttachParameters->FilterModuleGuidName->Buffer,
                      AttachParameters->FilterModuleGuidName->Length);
    }

    NdisAllocateSpinLock(&ctx->lock);
    KeInitializeEvent(&ctx->oid_complete_event, SynchronizationEvent, FALSE);
    RingInit(&ctx->ring);

    RtlZeroMemory(&attrs, sizeof(attrs));
    attrs.Header.Revision = NDIS_FILTER_ATTRIBUTES_REVISION_1;
    attrs.Header.Size     = NDIS_SIZEOF_FILTER_ATTRIBUTES_REVISION_1;
    attrs.Header.Type     = NDIS_OBJECT_TYPE_FILTER_ATTRIBUTES;
    attrs.Flags           = 0;

    status = NdisFSetAttributes(NdisFilterHandle, ctx, &attrs);
    if (status != NDIS_STATUS_SUCCESS) {
        RingFree(&ctx->ring);
        NdisFreeSpinLock(&ctx->lock);
        ExFreePoolWithTag(ctx, POOL_TAG);
        return status;
    }

    NdisAcquireSpinLock(&g.filter_list_lock);
    InsertTailList(&g.filter_list, &ctx->list_entry);
    NdisReleaseSpinLock(&g.filter_list_lock);

    InterlockedIncrement(&g.diag_attach_count);
    DiagWrite(L"AttachCount", (ULONG)g.diag_attach_count);
    DiagWrite(L"AttachMediaType", (ULONG)ctx->diag_media_type);
    MON_LOG("FilterAttach: ATTACHED to MT7921 adapter (IfIndex=%u), total=%d\n",
            ctx->if_index, g.diag_attach_count);
    return NDIS_STATUS_SUCCESS;
}

VOID FilterDetach(NDIS_HANDLE FilterModuleContext)
{
    PFILTER_CONTEXT ctx = (PFILTER_CONTEXT)FilterModuleContext;

    if (ctx->monitor_active)
        MonitorDisable(ctx);

    NdisAcquireSpinLock(&g.filter_list_lock);
    RemoveEntryList(&ctx->list_entry);
    NdisReleaseSpinLock(&g.filter_list_lock);

    RingFree(&ctx->ring);
    NdisFreeSpinLock(&ctx->lock);
    ExFreePoolWithTag(ctx, POOL_TAG);
}

NDIS_STATUS FilterRestart(NDIS_HANDLE FilterModuleContext,
                           PNDIS_FILTER_RESTART_PARAMETERS RestartParameters)
{
    UNREFERENCED_PARAMETER(FilterModuleContext);
    UNREFERENCED_PARAMETER(RestartParameters);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS FilterPause(NDIS_HANDLE FilterModuleContext,
                         PNDIS_FILTER_PAUSE_PARAMETERS PauseParameters)
{
    UNREFERENCED_PARAMETER(FilterModuleContext);
    UNREFERENCED_PARAMETER(PauseParameters);
    return NDIS_STATUS_SUCCESS;
}

VOID FilterReceiveNetBufferLists(NDIS_HANDLE FilterModuleContext,
                                  PNET_BUFFER_LIST NetBufferLists,
                                  NDIS_PORT_NUMBER PortNumber,
                                  ULONG NumberOfNetBufferLists,
                                  ULONG ReceiveFlags)
{
    PFILTER_CONTEXT ctx = (PFILTER_CONTEXT)FilterModuleContext;
    PNET_BUFFER_LIST nbl;

    if (ctx->monitor_active) {
        for (nbl = NetBufferLists; nbl; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
            PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl);
            while (nb) {
                CaptureFrame(ctx, nb, ctx->current_channel, 0);
                nb = NET_BUFFER_NEXT_NB(nb);
            }
        }
    }

    NdisFIndicateReceiveNetBufferLists(ctx->filter_handle,
                                       NetBufferLists,
                                       PortNumber,
                                       NumberOfNetBufferLists,
                                       ReceiveFlags);
}

/* ── Datapath pass-through handlers ──────────────────────────────
 * NDIS calls these with our FilterModuleContext; we forward using the
 * NDIS filter handle stored in the context. */
VOID FilterSendNetBufferLists(NDIS_HANDLE FilterModuleContext,
                              PNET_BUFFER_LIST NetBufferLists,
                              NDIS_PORT_NUMBER PortNumber,
                              ULONG SendFlags)
{
    PFILTER_CONTEXT ctx = (PFILTER_CONTEXT)FilterModuleContext;
    NdisFSendNetBufferLists(ctx->filter_handle, NetBufferLists, PortNumber, SendFlags);
}

VOID FilterSendNetBufferListsComplete(NDIS_HANDLE FilterModuleContext,
                                      PNET_BUFFER_LIST NetBufferLists,
                                      ULONG SendCompleteFlags)
{
    PFILTER_CONTEXT ctx = (PFILTER_CONTEXT)FilterModuleContext;
    NdisFSendNetBufferListsComplete(ctx->filter_handle, NetBufferLists, SendCompleteFlags);
}

VOID FilterReturnNetBufferLists(NDIS_HANDLE FilterModuleContext,
                                PNET_BUFFER_LIST NetBufferLists,
                                ULONG ReturnFlags)
{
    PFILTER_CONTEXT ctx = (PFILTER_CONTEXT)FilterModuleContext;
    NdisFReturnNetBufferLists(ctx->filter_handle, NetBufferLists, ReturnFlags);
}

VOID FilterCancelSendNetBufferLists(NDIS_HANDLE FilterModuleContext,
                                    PVOID CancelId)
{
    PFILTER_CONTEXT ctx = (PFILTER_CONTEXT)FilterModuleContext;
    NdisFCancelSendNetBufferLists(ctx->filter_handle, CancelId);
}

/* ── OID request pass-through (clone + forward) ──────────────────── */
static VOID CopyOidResults(PNDIS_OID_REQUEST dst, PNDIS_OID_REQUEST src)
{
    switch (src->RequestType) {
    case NdisRequestSetInformation:
        dst->DATA.SET_INFORMATION.BytesRead   = src->DATA.SET_INFORMATION.BytesRead;
        dst->DATA.SET_INFORMATION.BytesNeeded = src->DATA.SET_INFORMATION.BytesNeeded;
        break;
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        dst->DATA.QUERY_INFORMATION.BytesWritten = src->DATA.QUERY_INFORMATION.BytesWritten;
        dst->DATA.QUERY_INFORMATION.BytesNeeded  = src->DATA.QUERY_INFORMATION.BytesNeeded;
        break;
    case NdisRequestMethod:
        dst->DATA.METHOD_INFORMATION.OutputBufferLength = src->DATA.METHOD_INFORMATION.OutputBufferLength;
        dst->DATA.METHOD_INFORMATION.BytesRead    = src->DATA.METHOD_INFORMATION.BytesRead;
        dst->DATA.METHOD_INFORMATION.BytesNeeded  = src->DATA.METHOD_INFORMATION.BytesNeeded;
        dst->DATA.METHOD_INFORMATION.BytesWritten = src->DATA.METHOD_INFORMATION.BytesWritten;
        break;
    default:
        break;
    }
}

NDIS_STATUS FilterOidRequest(NDIS_HANDLE FilterModuleContext,
                             PNDIS_OID_REQUEST OidRequest)
{
    PFILTER_CONTEXT     ctx   = (PFILTER_CONTEXT)FilterModuleContext;
    PNDIS_OID_REQUEST   clone = NULL;
    NDIS_STATUS         status;

    status = NdisAllocateCloneOidRequest(ctx->filter_handle, OidRequest,
                                         POOL_TAG, &clone);
    if (status != NDIS_STATUS_SUCCESS)
        return status;

    /* Remember the original so the completion handler can return it */
    *((PNDIS_OID_REQUEST *)(&clone->SourceReserved[0])) = OidRequest;

    status = NdisFOidRequest(ctx->filter_handle, clone);
    if (status != NDIS_STATUS_PENDING) {
        /* Completed inline — copy results back and free the clone.
         * NDIS completes the original request with our return status. */
        CopyOidResults(OidRequest, clone);
        NdisFreeCloneOidRequest(ctx->filter_handle, clone);
    }
    return status;
}

VOID FilterOidRequestComplete(NDIS_HANDLE FilterModuleContext,
                               PNDIS_OID_REQUEST OidRequest,
                               NDIS_STATUS Status)
{
    PFILTER_CONTEXT   ctx = (PFILTER_CONTEXT)FilterModuleContext;
    PNDIS_OID_REQUEST original;

    /* Our own internally-originated request (from monitor.c)? */
    if (OidRequest == &ctx->oid_request) {
        ctx->oid_status = Status;
        KeSetEvent(&ctx->oid_complete_event, IO_NO_INCREMENT, FALSE);
        return;
    }

    /* Otherwise it is a clone of a pass-through request that pended */
    original = *((PNDIS_OID_REQUEST *)(&OidRequest->SourceReserved[0]));
    CopyOidResults(original, OidRequest);
    NdisFreeCloneOidRequest(ctx->filter_handle, OidRequest);
    NdisFOidRequestComplete(ctx->filter_handle, original, Status);
}

PFILTER_CONTEXT FindContextByIfIndex(NET_IFINDEX idx)
{
    PLIST_ENTRY     entry;
    PFILTER_CONTEXT ctx = NULL;

    NdisAcquireSpinLock(&g.filter_list_lock);
    for (entry = g.filter_list.Flink;
         entry != &g.filter_list;
         entry = entry->Flink) {
        PFILTER_CONTEXT c = CONTAINING_RECORD(entry, FILTER_CONTEXT, list_entry);
        if (c->if_index == idx) { ctx = c; break; }
    }
    NdisReleaseSpinLock(&g.filter_list_lock);
    return ctx;
}
