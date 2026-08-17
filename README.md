# MT7921 Windows Monitor-Mode Driver — a systems-research experiment

An **NDIS Lightweight Filter (LWF)** driver that attempts to enable **802.11 monitor mode** (raw frame capture) on the internal **MediaTek MT7921** Wi-Fi adapter under **Windows 11** — a capability Windows does not expose natively for this chipset.

> ### ⚠️ Research / educational — with a definitive, *measured* conclusion
> The driver **compiles, loads, runs, and attaches** to the MT7921 adapter — verified on real hardware (the `mt7921mon` kernel service reports **RUNNING**). Yet **802.11 monitor mode is impossible through this approach, and now proven so empirically:** the closed MediaTek miniport **rejects the monitor-mode OID with `NDIS_STATUS_NOT_ACCEPTED` (`0xC0010017`)** and only ever exposes **802.3-emulated** edges — never raw Native 802.11. The wall is **not** driver-signing (the driver loads fine); it is the **vendor miniport refusing to expose the capability**, which a filter driver cannot add. Published as a **documented systems-research write-up with real measurements**, not a finished tool. **For authorized security auditing and education only.** See [Safety & legal](#safety--legal).

---

## Why monitor mode is hard on Windows (and easy on Linux)

On Linux, the **mt76** driver is open source and plugs into the **mac80211** stack, so `iw dev … set monitor` and channel hopping "just work." Windows has none of that:

- The MediaTek Wi-Fi driver (miniport) is **closed** and exposes only the standard **Native Wi-Fi** surface, whose "Network Monitor" operating mode is **gated by the vendor driver** and rarely enabled for consumer parts.
- There is **no documented raw-injection / radiotap path** in the Windows Wi-Fi stack.
- Loading any custom kernel driver runs into **Driver Signature Enforcement (DSE)**, **HVCI / Memory Integrity**, and — on recent builds — **Smart App Control (SAC)**.

This project explores how far a **filter driver** can push monitor mode *without* replacing the vendor miniport.

## Architecture

An NDIS LWF inserts itself between the protocol stack and the MT7921 miniport, so it can both **issue OIDs down to the hardware** and **observe every received frame** on the way up:

```
 user space  ──IOCTL──►  \\.\mt7921mon (control device)
                              │
      Protocol drivers        │  enable / channel / read-packet
            ▲                 ▼
     ┌──────────────  mt7921mon (NDIS LWF)  ──────────────┐
     │  RX: tap frames → ring buffer   TX/OID: pass-through │
     └───────────────────────┬─────────────────────────────┘
                             ▼
                  MT7921 miniport (closed) → firmware → radio
```

**Monitor-enable is attempted in three layers** ([`src/monitor.c`](src/monitor.c)):

1. **NDIS Native Wi-Fi** — `OID_DOT11_CURRENT_OPERATION_MODE` → `DOT11_OPERATION_MODE_NETWORK_MONITOR`.
2. **Raw MediaTek firmware command** (fallback) — `MCU_UNI_CMD_SNIFFER` pushed through `OID_DOT11_NIC_SPECIFIC_EXTENSION`, mirroring the sniffer path in the Linux **mt76** driver.
3. **Raw packet filter** — `OID_GEN_CURRENT_PACKET_FILTER` with `PROMISCUOUS | 802_11_RAW_DATA | 802_11_RAW_MGMT`.

Captured frames are copied into a 2 MB lock-protected **ring buffer** ([`src/packet.c`](src/packet.c)) with a radiotap-style header (channel, RSSI, rate, timestamp) and drained to user space over `IOCTL_READ_PACKET`.

## The interesting bug: `NDIS_STATUS_BAD_CHARACTERISTICS` (0xC0010005)

Registration first failed with `0xC0010005`. The cause is a subtle NDIS contract:

- Declaring filter characteristics **REVISION_3** makes the **synchronous-OID** handlers *mandatory*, but this filter forwards OIDs asynchronously — NDIS rejects the mismatch.
- Because the driver **originates its own OID requests** (to switch mode/channel), it must supply the **`OidRequest` *and* `OidRequestComplete` handlers as a pair**, with a proper **clone-and-forward** implementation.

The fix ([`src/filter.c`](src/filter.c)): target **`NDIS_FILTER_CHARACTERISTICS_REVISION_2`** (NDIS 6.30), and implement OID pass-through by cloning each request (`NdisAllocateCloneOidRequest`), forwarding it, and reconciling results in the completion handler — while a sentinel (`OidRequest == &ctx->oid_request`) distinguishes the driver's own internally-originated OIDs from pass-through traffic.

## Debugging a boot-time driver without a kernel debugger

`DbgPrint` isn't visible while a network filter initialises at boot, so the driver writes **progress checkpoints to the registry** (`HKLM\SOFTWARE\mt7921mon`) from `DriverEntry` and `FilterAttach`, and exposes every OID's status through an `IOCTL_GET_STATUS` struct ([`MONITOR_STATUS`](src/filter.h)). From user space you can then read *exactly* how far initialisation got and which OID the vendor driver rejected — no WinDbg required. The included [`read_diag.py`](read_diag.py) reads both, and is exactly what produced the measurements in [Status / results](#status--results) below.

## Status / results

| Stage | Result |
|-------|--------|
| Compiles (WDK, x64) → `mt7921mon.sys` | ✅ |
| Registers as NDIS filter, creates `\\.\mt7921mon` | ✅ (`RegisterStatus = 0`) |
| **Loads & runs** on Windows 11 (with test-signing) | ✅ **verified — service `RUNNING`** |
| Attaches to the adapter | ✅ but only to **802.3-emulated** edges (`media type 0`, 12 attaches) |
| Attaches to a **raw Native 802.11** edge | ❌ **never** (no `NativeWifiIfIndex` ever recorded) |
| Vendor miniport **accepts** the monitor-mode OID | ❌ **rejected — `NDIS_STATUS_NOT_ACCEPTED` (`0xC0010017`)** |
| End-to-end raw 802.11 capture | ❌ **impossible via a filter driver** (proven) |

### Measured on real hardware — the proof

The driver records boot-time checkpoints to `HKLM\SOFTWARE\mt7921mon` and exposes every OID's result through `IOCTL_GET_STATUS`. [`read_diag.py`](read_diag.py) reads both. On the test machine, with the driver **loaded and RUNNING**, it reported:

```text
service state       : RUNNING
RegisterStatus      : 0x00000000  (SUCCESS)
AttachCount         : 12          (adapter edges bound)
LastAttachMediaType : 0           -> 802.3-emulated  (never Native 802.11 / raw)
NativeWifiIfIndex   : (absent)    -> a raw 802.11 edge was never offered
OID_DOT11_CURRENT_OPERATION_MODE : 0xC0010017  (NDIS_STATUS_NOT_ACCEPTED — REJECTED)
MCU_UNI_CMD_SNIFFER via NIC ext  : 0xC0010017  (REJECTED)
OID_GEN_CURRENT_PACKET_FILTER    : 0x00000000  (accepted — meaningless without monitor)
```

The filter registers, loads, runs and binds — but the miniport **refuses the monitor-mode request outright** and never hands up a raw 802.11 edge. That is the entire finding, in NDIS status codes.

### Why the filter approach is a dead end
1. **Closed miniport (the real wall).** Unlike Linux `mt76`, the Windows MediaTek driver exposes no monitor path and **actively rejects** the monitor OID (`0xC0010017`). A filter driver sits *above* the miniport — it cannot add a capability the miniport does not expose.
2. **Only 802.3-emulated frames reach the filter** — never raw 802.11 with a radiotap/PHY header — so even passive tapping yields Ethernet-framed data, not monitor-mode captures.
3. **Driver signing is *not* the blocker** — the driver loaded and ran. Signing (test-signing + HVCI/Memory-Integrity off) is a surmountable, reversible dev setting; it was surmounted here. The wall is the vendor driver, not platform policy.

**Conclusion (measured, not theorised):** the filter logic works and the driver runs — but the **closed MediaTek miniport simply refuses monitor mode** and never exposes raw 802.11, so a filter driver *cannot* achieve it. The only remaining native route is a **full custom miniport** (porting `mt76` to Windows) — an enormous effort that *then* meets the signing wall. The practical alternatives: **[Npcap](https://npcap.com/)** for passive raw-802.11 capture (adapter-dependent), or a **userspace WinUSB/libusb driver** for a USB adapter (bypasses the miniport entirely). This repo documents the filter-driver dead-end in full — with measurements — so others don't spend weeks rediscovering it.

## Build & test

See [BUILD.md](BUILD.md). In short: Visual Studio 2022 + WDK, `msbuild src/mt7921mon.vcxproj /p:Configuration=Release /p:Platform=x64`, then load on a **test-signing-enabled, non-production** machine. No prebuilt `.sys` is shipped — see below.

## Safety & legal

- This is **kernel-mode** code. A bug can **bounce your machine (BSOD)**; run it only in a VM or a spare, non-production install.
- Loading it requires **weakening Windows security** (test-signing; disabling SAC/Memory Integrity). Understand and reverse those changes afterwards.
- **No compiled binary is distributed** — build from source deliberately, on a machine you own.
- Use only on networks and devices **you own or are explicitly authorised to test**. Monitor mode / packet capture may be regulated in your jurisdiction.

## References

- MediaTek **mt76** Linux driver — monitor / `MCU_UNI_CMD_SNIFFER` path
- Microsoft — *NDIS Lightweight Filter Drivers* and *Native 802.11 Wireless LAN* OID reference (`OID_DOT11_*`)
- Microsoft — *Driver Signing*, *Device Guard / HVCI*, *Smart App Control*

## License

[Apache License 2.0](LICENSE) — © 2026 Ahmad Al-Ahmad, see [NOTICE](NOTICE). The kernel-driver disclaimer in [Safety & legal](#safety--legal) applies. Companion project: a native Windows build of hcxtools (see my other repositories).
