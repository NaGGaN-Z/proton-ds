# Gadget Bridge Protocol — daemon ↔ gadget-shim

Status: draft v1 (plan T1, Phase 0) · Owner: proton-ds v0.2 E3 path

## 1. Purpose

Phase 2 of the E3 path: the ds4linux daemon owns pad input (BT or USB,
PR#3 translations, rumble handling) and streams it to the root-run
gadget-shim, which owns the USB gadget device (054c:09cc) presented to
wine. One socket, raw reports, zero translation in the shim.

Design rule (fixed in plan): **the wire format for the input stream is
the final 64-byte DS4 USB input report.** The daemon serializes
`DS4InputState` with its existing serializer (`VirtualDevice::send_report`,
`virtual_device.cpp` — the same bytes it would pass to `UHID_INPUT2`);
the shim forwards them verbatim to ep1. No re-encoding, no JSON, no
per-field marshalling on the hot path.

## 2. Channel

| | |
|---|---|
| Transport | `AF_UNIX SOCK_SEQPACKET`… see §7 decision: `SOCK_STREAM` |
| Socket path | `/run/ds4linux-bridge.sock` (default; daemon flag `--bridge-socket`) |
| Owner | daemon in `--gadget-bridge` mode (unlink stale, bind, mode 0600 — root-only: daemon and shim both run under sudo per root-model) |
| Clients | exactly one (the shim); second connect → refused |
| Relation to GUI IPC | fully separate from the JSON socket `/run/ds4linux.sock` (`kSocketPath`, constants.h). GUI IPC untouched — no multiplexing of 250 Hz binary into the JSON channel |

## 3. Framing

Same length-prefix convention as the JSON IPC (`encode_message`), but
payload is raw bytes, not JSON:

```
[ uint32 LE length ][ uint8 type ][ data ... ]     length counts type+data
```

## 4. Frame types

| Type | Dir | Name | data | Notes |
|---|---|---|---|---|
| 0x01 | D→S | INPUT | 64 B | Final DS4 report, byte 0 = report ID 0x01. Verbatim ep1 write. |
| 0x02 | S→D | OUTPUT | raw DS4 output report (32 B USB `0x05`) | Bytes as received on ep2. Daemon maps `valid_flag0/1` → DS5 output via its existing rumble/lightbar path. BT `0x31` never crosses this bridge (see §6). |
| 0x03 | D→S | PAD_INFO | `[u8 transport (0=USB, 1=BT)][6 B MAC]` | On daemon→shim connect (if pad present) and on hot-plug. MAC feeds gadget serial + 0x12/0x81 answers. |
| 0x04 | D→S | PAD_GONE | — | Pad lost. Shim zeroes reports, keeps gadget alive (mirrors T4 standalone degradation). |
| 0x05 | S→D | GET_CALIB | — | **Bonus, not in MVP.** Shim requests real-pad calibration forward. |
| 0x06 | D→S | CALIB | DS4 0x02 layout blob | Reply to 0x05. MVP shim synthesizes 0x02 instead (as daemon does today). |

## 5. Rate & backpressure

- Input is **latest-state semantics**: each frame supersedes the previous.
  Daemon sends every report it would have written to uhid (~100–250 Hz).
  Socket writes are non-blocking; on `EAGAIN` the frame is **dropped**
  (the next report is fresher anyway). No user-space queues on either
  side beyond socket buffers.
- Overhead check: 69 B/frame @ 250 Hz ≈ 17 KB/s — negligible for a unix
  socket.
- Output (S→D) is low-rate (rumble/lightbar changes only). Blocking
  write acceptable; daemon applies latest-wins if frames back up.
- Reconnect: on disconnect shim retries with backoff 250 ms → 2 s;
  daemon keeps reading the pad regardless.

## 6. Transport scope

Bridge input works for USB and BT pads identically (daemon's evdev
layer normalizes). Bridge output carries only the USB DS4 output report
(`0x05`): the gadget is a USB device, wine only ever sends `0x05`/`0x11`-
SET_REPORT on it. The DS5-side output (USB `0x02` 63 B / BT `0x31`+CRC32
78 B) is chosen by the **daemon** from the pad's actual transport — the
shim knows nothing about it.

## 7. Open points resolved in this draft

1. **Same socket vs separate**: separate. JSON GUI clients must not
   parse binary frames; the stream channel is root-only anyway.
2. **SOCK_SEQPACKET vs SOCK_STREAM**: `SOCK_STREAM` — one message type
   per direction dominates and length-prefix framing already exists;
   SEQPACKET would buy nothing but portability risk.
3. **Versioning**: none on the wire. Shim and bridge-daemon ship as one
   proton-ds release set (ds4ctl starts both); protocol changes bump
   both together.

## 8. Daemon mode

`ds4linux-daemon --gadget-bridge [--bridge-socket PATH]`:

- disables all three virtual devices (uhid gamepad `UHID_CREATE2`,
  uinput VirtualTouchpad, uinput VirtualMotion) — no evdev ghosts;
- keeps pad input, translations, profiles, GUI JSON IPC working;
- `send_report` sink switches: `UHID_INPUT2` → bridge frame 0x01;
- includes the `cr.version = 0x0100` fix (correct for every mode).
