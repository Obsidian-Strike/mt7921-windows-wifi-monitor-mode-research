/*
 * mt7921_fw.h - MT7921 firmware command definitions
 * Derived from Linux mt76 driver (GPL-2.0)
 * Reference: apriorit/mt7921, torvalds/linux mt76/mt7921
 */
#pragma once

/* ── MCU Unified Command IDs ─────────────────────────────────── */
#define MCU_UNI_CMD_SNIFFER         0x24
#define MCU_UNI_CMD_BSS_INFO_UPDATE 0x16
#define MCU_UNI_CMD_STA_REC_UPDATE  0x15

/* ── Sniffer TLV Tags ────────────────────────────────────────── */
#define SNIFFER_TLV_ENABLE          0   /* enable/disable sniffer */
#define SNIFFER_TLV_CONFIG          1   /* channel config         */

/* ── Band encoding ───────────────────────────────────────────── */
#define MT7921_BAND_24G             1
#define MT7921_BAND_5G              2
#define MT7921_BAND_6G              3

/* ── Bandwidth encoding ──────────────────────────────────────── */
#define MT7921_BW_20                0
#define MT7921_BW_40                1
#define MT7921_BW_80                2
#define MT7921_BW_160               3

/* ── SCO (Secondary Channel Offset) ─────────────────────────── */
#define MT7921_SCO_NONE             0
#define MT7921_SCO_SCA              1   /* secondary channel above */
#define MT7921_SCO_SCB              3   /* secondary channel below */

/* ── MCU message header ──────────────────────────────────────── */
#pragma pack(push, 1)

typedef struct _MCU_UNI_HDR {
    UINT8  reserved[4];
    UINT16 len;         /* payload length (bytes after this header) */
    UINT8  cmd;         /* MCU_UNI_CMD_* */
    UINT8  seq;         /* sequence number */
} MCU_UNI_HDR;

/* ── TLV base ────────────────────────────────────────────────── */
typedef struct _MCU_TLV_HDR {
    UINT16 tag;
    UINT16 len;         /* sizeof entire TLV struct */
} MCU_TLV_HDR;

/* ── SNIFFER enable TLV (tag = SNIFFER_TLV_ENABLE) ──────────── */
typedef struct _SNIFFER_ENABLE_TLV {
    MCU_TLV_HDR hdr;   /* tag=0, len=sizeof(this) */
    UINT8  enable;      /* 1 = enable, 0 = disable  */
    UINT8  pad[3];
} SNIFFER_ENABLE_TLV;

/* ── SNIFFER config TLV (tag = SNIFFER_TLV_CONFIG) ──────────── */
typedef struct _SNIFFER_CONFIG_TLV {
    MCU_TLV_HDR hdr;   /* tag=1, len=sizeof(this) */
    UINT16 aid;
    UINT8  ch_band;     /* MT7921_BAND_* */
    UINT8  bw;          /* MT7921_BW_*   */
    UINT8  control_ch;  /* primary channel number */
    UINT8  sco;         /* MT7921_SCO_*  */
    UINT8  center_ch;
    UINT8  center_ch2;  /* for 80+80 MHz */
    UINT8  drop_err;    /* drop erroneous frames */
    UINT8  pad[3];
} SNIFFER_CONFIG_TLV;

/* ── Full MCU sniffer enable message ────────────────────────── */
typedef struct _MCU_SNIFFER_ENABLE_MSG {
    MCU_UNI_HDR       hdr;
    SNIFFER_ENABLE_TLV tlv;
} MCU_SNIFFER_ENABLE_MSG;

/* ── Full MCU sniffer config message ────────────────────────── */
typedef struct _MCU_SNIFFER_CONFIG_MSG {
    MCU_UNI_HDR        hdr;
    SNIFFER_CONFIG_TLV tlv;
} MCU_SNIFFER_CONFIG_MSG;

#pragma pack(pop)

/* ── Helper: channel → band ──────────────────────────────────── */
static inline UINT8 ChannelToBand(UINT8 ch) {
    if (ch <= 14)  return MT7921_BAND_24G;
    if (ch <= 177) return MT7921_BAND_5G;
    return MT7921_BAND_6G;
}
