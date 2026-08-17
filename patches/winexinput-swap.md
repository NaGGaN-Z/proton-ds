# winexinput.sys — IG_00 ↔ XI_00 swap (data hex-patch)

## Why

winexinput creates two child PDOs per gamepad FDO (`create_child_pdos`):

| Child | device_id suffix | is_gamepad | IOCTL_HID_* behavior |
|---|---|---|---|
| gamepad | `&IG_00` | TRUE | **synthetic** in-tree XInput descriptor (by design) |
| xinput | `&XI_00` | FALSE | **passthrough** to the bus parent → REAL descriptor |

Strict DS4-aware games (libScePad) open the `IG_00` interface path and the
WMI gate matches `IG_` in DeviceID — but that child serves the synthetic
descriptor. The swap puts the real descriptor on the IG_00 path and moves
the synthetic one to XI_00 (where XInput-style consumers of the twin would
look — and where `hidclass` GUID patch then hides it entirely).

## Recipe (python3, apply to x86_64 + i386 dist copies; engine: scripts/hexpatch.py)

```python
IG = b"&\x00I\x00G\x00_\x000\x000\x00"   # UTF-16LE "&IG_00"
XI = b"&\x00X\x00I\x00_\x000\x000\x00"   # UTF-16LE "&XI_00"
PLACEHOLDER = bytes([0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x99,0x88,0x77,0x66,0x55,0x44])

data = open(path, "rb").read()
assert data.count(IG) == 1, f"IG occurrences != 1 in {path}"
assert data.count(XI) == 1, f"XI occurrences != 1 in {path}"
d = data.replace(IG, PLACEHOLDER)      # NEVER a zero placeholder — it
d = d.replace(XI, IG)                  # matches padding windows and
d = d.replace(PLACEHOLDER, XI)         # corrupts the binary (verified bug)
assert d.count(IG) == 1 and d.count(XI) == 1 and len(d) == len(data)
open(path + ".swbak", "wb").write(data)   # rollback next to target
open(path, "wb").write(d)
```

## Targets

- `<GE>/files/lib/wine/x86_64-windows/winexinput.sys`
- `<GE>/files/lib/wine/i386-windows/winexinput.sys`

NOTE: game-prefix copies are NOT patched — wineboot propagates the driver
from the dist of the instance a game runs on (hash-audited 2026-08-17:
BG3/DS:DC prefix copies became patched automatically after launching on
the -DS instance, Witcher stayed stock after running on stock). A prefix
refreshes on the first launch on the new instance.
