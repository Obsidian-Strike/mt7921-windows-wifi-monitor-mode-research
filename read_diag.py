r"""
mt7921mon - diagnostic reader.

Run AFTER the driver is installed/loaded to see, empirically, how far it got:
  • registry checkpoints written by DriverEntry / FilterAttach
  • runtime MONITOR_STATUS via IOCTL on \\.\mt7921mon (if the control device exists)

Usage:  python read_diag.py     (Administrator recommended)
"""

import sys
import ctypes
from ctypes import wintypes

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

try:
    import winreg
except Exception:
    winreg = None

REG_PATH = r"SOFTWARE\mt7921mon"

STAGE = {0: "not entered / key just created", 1: "DriverEntry entered",
         2: "filter registered OK", 3: "FULL SUCCESS (loaded + control device)",
         98: "FAILED at CreateControlDevice", 99: "FAILED at NdisFRegisterFilterDriver"}

# NDIS media types we care about
MEDIA = {0: "802.3 (Ethernet-emulated)", 16: "Native 802.11 (RAW — monitor-capable edge!)",
         0xFFFFFFFF: "none seen yet"}


def read_registry():
    print("── Registry checkpoints (HKLM\\SOFTWARE\\mt7921mon) ──")
    if not winreg:
        print("  winreg unavailable"); return
    try:
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, REG_PATH)
    except FileNotFoundError:
        print("  key missing — driver never ran (or key not pre-created)"); return
    vals = {}
    i = 0
    while True:
        try:
            name, val, _ = winreg.EnumValue(k, i)
            vals[name] = val
            i += 1
        except OSError:
            break
    stage = vals.get("Stage")
    reg = vals.get("RegisterStatus")
    print(f"  Stage            : {stage}  → {STAGE.get(stage, 'unknown')}")
    if reg is not None:
        s = reg & 0xFFFFFFFF
        print(f"  RegisterStatus   : 0x{s:08X}  ({'SUCCESS' if s == 0 else 'NDIS error'})")
    print(f"  CtrlDevStatus    : {_hex(vals.get('CtrlDevStatus'))}")
    print(f"  AttachAttempts   : {vals.get('AttachAttempts', 0)}  (times NDIS offered an adapter)")
    print(f"  AttachCount      : {vals.get('AttachCount', 0)}  (adapters we actually bound)")
    mt = vals.get("LastAttachMediaType")
    print(f"  LastAttachMedia  : {mt}  → {MEDIA.get(mt & 0xFFFFFFFF if mt is not None else None, 'other/'+str(mt))}")
    if vals.get("NativeWifiIfIndex") is not None:
        print(f"  NativeWifiIfIndex: {vals['NativeWifiIfIndex']}  ← a raw Native-802.11 edge exists! (monitor viable)")
    # verdict
    print()
    if stage == 3 and vals.get("AttachCount", 0) > 0:
        print("  VERDICT: driver loaded AND attached to an adapter. Monitor path is worth trying.")
    elif stage == 3:
        print("  VERDICT: driver loaded, but attached to 0 adapters yet (NDIS didn't offer the Wi-Fi edge).")
    elif stage in (98, 99):
        print("  VERDICT: driver failed to initialize — see status codes above.")
    else:
        print("  VERDICT: driver has not fully loaded (Stage < 3). Check install + test-signing/HVCI.")


def _hex(v):
    return f"0x{v & 0xFFFFFFFF:08X}" if isinstance(v, int) else str(v)


def read_ioctl():
    print("\n── Runtime status via \\\\.\\mt7921mon (IOCTL_GET_STATUS) ──")
    GENERIC_RW = 0xC0000000
    OPEN_EXISTING = 3
    INVALID = ctypes.c_void_p(-1).value
    # CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, func=0x8004, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
    IOCTL_GET_STATUS = (0x22 << 16) | (0x8004 << 2)
    k32 = ctypes.windll.kernel32
    h = k32.CreateFileW(r"\\.\mt7921mon", GENERIC_RW, 0, None, OPEN_EXISTING, 0, None)
    if h == INVALID:
        print(f"  control device not open (err {ctypes.get_last_error()}) — driver not loaded, or no \\.\\mt7921mon")
        return
    try:
        buf = ctypes.create_string_buffer(44)      # sizeof(MONITOR_STATUS), pack(1)
        ret = wintypes.DWORD(0)
        ok = k32.DeviceIoControl(h, IOCTL_GET_STATUS, None, 0, buf, 44, ctypes.byref(ret), None)
        if not ok:
            print(f"  IOCTL failed (err {ctypes.get_last_error()})"); return
        import struct
        # MONITOR_STATUS, pack(1): 4×UINT8, 2×UINT32, 2×INT32, 2×UINT32, 4×INT32
        (active, channel, bw, _r, cap, drop, dreg, dctrl, datt, dmedia,
         dndis, dmcu, dpkt, dchan) = struct.unpack("<BBBBIIiiIIiiii", buf.raw[:44])
        print(f"  monitor_active   : {active}")
        print(f"  packets captured : {cap}  dropped: {drop}")
        print(f"  diag_last_ndis_mode   (OID_DOT11_CURRENT_OPERATION_MODE): 0x{dndis & 0xFFFFFFFF:08X}")
        print(f"  diag_last_mcu_sniffer (MCU sniffer via NIC ext)         : 0x{dmcu & 0xFFFFFFFF:08X}")
        print(f"  diag_last_pkt_filter  (OID_GEN_CURRENT_PACKET_FILTER)   : 0x{dpkt & 0xFFFFFFFF:08X}")
        print("  (0x00000000 = the miniport accepted it; non-zero = rejected)")
    finally:
        k32.CloseHandle(h)


if __name__ == "__main__":
    ctypes.windll.kernel32.SetLastError(0)
    print("mt7921mon diagnostics\n")
    read_registry()
    read_ioctl()
