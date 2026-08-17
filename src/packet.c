/*
 * packet.c - Ring buffer + 802.11 frame capture
 */
#include "filter.h"

/* ── RingInit ────────────────────────────────────────────────── */
VOID RingInit(PRING_BUFFER ring)
{
    ring->buf  = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool,
                                               RING_BUF_SIZE, 'gnir');
    ring->size = ring->buf ? RING_BUF_SIZE : 0;
    ring->head = 0;
    ring->tail = 0;
    KeInitializeEvent(&ring->data_event, NotificationEvent, FALSE);
    KeInitializeSpinLock(&ring->lock);
}

/* ── RingFree ────────────────────────────────────────────────── */
VOID RingFree(PRING_BUFFER ring)
{
    if (ring->buf) {
        ExFreePoolWithTag(ring->buf, 'gnir');
        ring->buf = NULL;
    }
}

/* ── RingWrite: write [len] bytes atomically ─────────────────── */
BOOLEAN RingWrite(PRING_BUFFER ring, PUCHAR data, UINT32 len)
{
    KIRQL   irql;
    UINT32  head, tail, avail, needed;
    UINT32  first_chunk, second_chunk;

    if (!ring->buf || len == 0) return FALSE;

    needed = sizeof(UINT32) + len;     /* length prefix + data */

    KeAcquireSpinLock(&ring->lock, &irql);

    head  = (UINT32)ring->head;
    tail  = (UINT32)ring->tail;
    avail = (head >= tail)
          ? (ring->size - head + tail)
          : (tail - head);

    if (avail < needed + 1) {          /* +1 to avoid head==tail ambiguity */
        KeReleaseSpinLock(&ring->lock, irql);
        return FALSE;                  /* drop */
    }

    /* Write 4-byte length prefix */
    first_chunk  = min(sizeof(UINT32), ring->size - head);
    second_chunk = sizeof(UINT32) - first_chunk;
    RtlCopyMemory(ring->buf + head, &len, first_chunk);
    if (second_chunk)
        RtlCopyMemory(ring->buf, ((PUCHAR)&len) + first_chunk, second_chunk);
    head = (head + sizeof(UINT32)) % ring->size;

    /* Write data */
    first_chunk  = min(len, ring->size - head);
    second_chunk = len - first_chunk;
    RtlCopyMemory(ring->buf + head, data, first_chunk);
    if (second_chunk)
        RtlCopyMemory(ring->buf, data + first_chunk, second_chunk);
    head = (head + len) % ring->size;

    InterlockedExchange(&ring->head, (LONG)head);
    KeSetEvent(&ring->data_event, IO_NETWORK_INCREMENT, FALSE);

    KeReleaseSpinLock(&ring->lock, irql);
    return TRUE;
}

/* ── RingRead: read one packet from ring ─────────────────────── */
UINT32 RingRead(PRING_BUFFER ring, PUCHAR out, UINT32 max_len)
{
    KIRQL  irql;
    UINT32 head, tail, used, pkt_len;
    UINT32 first_chunk, second_chunk;

    if (!ring->buf) return 0;

    KeAcquireSpinLock(&ring->lock, &irql);

    head = (UINT32)ring->head;
    tail = (UINT32)ring->tail;

    if (head == tail) {                /* empty */
        KeReleaseSpinLock(&ring->lock, irql);
        return 0;
    }

    /* Read 4-byte length prefix */
    first_chunk  = min(sizeof(UINT32), ring->size - tail);
    second_chunk = sizeof(UINT32) - first_chunk;
    RtlCopyMemory(&pkt_len, ring->buf + tail, first_chunk);
    if (second_chunk)
        RtlCopyMemory(((PUCHAR)&pkt_len) + first_chunk, ring->buf, second_chunk);
    tail = (tail + sizeof(UINT32)) % ring->size;

    if (pkt_len > max_len) {           /* caller buffer too small — skip */
        tail = (tail + pkt_len) % ring->size;
        InterlockedExchange(&ring->tail, (LONG)tail);
        KeReleaseSpinLock(&ring->lock, irql);
        return 0;
    }

    /* Read data */
    first_chunk  = min(pkt_len, ring->size - tail);
    second_chunk = pkt_len - first_chunk;
    RtlCopyMemory(out, ring->buf + tail, first_chunk);
    if (second_chunk)
        RtlCopyMemory(out + first_chunk, ring->buf, second_chunk);
    tail = (tail + pkt_len) % ring->size;

    InterlockedExchange(&ring->tail, (LONG)tail);

    /* Reset event if ring is now empty */
    used = (head >= tail) ? (head - tail) : (ring->size - tail + head);
    if (used == 0) KeClearEvent(&ring->data_event);

    KeReleaseSpinLock(&ring->lock, irql);
    return pkt_len;
}

/* ── CaptureFrame: copy NET_BUFFER into ring with header ─────── */
VOID CaptureFrame(PFILTER_CONTEXT ctx, PNET_BUFFER nb,
                  UINT8 channel, INT8 rssi)
{
    CAPTURE_PKT_HDR hdr;
    PUCHAR          pkt_data;
    UINT32          data_len, total_len;
    LARGE_INTEGER   ts;
    PUCHAR          flat;

    data_len = NET_BUFFER_DATA_LENGTH(nb);
    if (data_len == 0 || data_len > MAX_PKT_SIZE) {
        InterlockedIncrement((LONG*)&ctx->packets_dropped);
        return;
    }

    total_len = sizeof(CAPTURE_PKT_HDR) + data_len;

    /* Flatten MDL chain into contiguous buffer */
    flat = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, total_len, 'pakc');
    if (!flat) {
        InterlockedIncrement((LONG*)&ctx->packets_dropped);
        return;
    }

    KeQuerySystemTimePrecise(&ts);

    /* Build capture header */
    hdr.magic       = CAPTURE_PKT_MAGIC;
    hdr.caplen      = (UINT16)data_len;
    hdr.origlen     = (UINT16)data_len;
    hdr.timestamp_us = (UINT64)(ts.QuadPart / 10); /* 100ns → µs */
    hdr.channel     = channel;
    hdr.rssi        = (UINT8)rssi;
    hdr.data_rate   = 0;
    hdr.flags       = 0;

    RtlCopyMemory(flat, &hdr, sizeof(CAPTURE_PKT_HDR));

    /* Copy frame data from MDL chain */
    pkt_data = NdisGetDataBuffer(nb, data_len,
                                 flat + sizeof(CAPTURE_PKT_HDR),
                                 1, 0);
    if (!pkt_data) {
        /* NdisGetDataBuffer returned NULL — data already in our buffer? */
        /* Copy manually via MmGetSystemAddressForMdlSafe if needed */
        ExFreePoolWithTag(flat, 'pakc');
        InterlockedIncrement((LONG*)&ctx->packets_dropped);
        return;
    }

    /* If pkt_data != flat+sizeof(hdr), the data is elsewhere — copy it */
    if (pkt_data != flat + sizeof(CAPTURE_PKT_HDR)) {
        RtlCopyMemory(flat + sizeof(CAPTURE_PKT_HDR), pkt_data, data_len);
    }

    if (RingWrite(&ctx->ring, flat, total_len)) {
        InterlockedIncrement((LONG*)&ctx->packets_captured);
    } else {
        InterlockedIncrement((LONG*)&ctx->packets_dropped);
    }

    ExFreePoolWithTag(flat, 'pakc');
}
