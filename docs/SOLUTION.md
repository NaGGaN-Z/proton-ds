# The Solution Stack (2026-08-15, verified 4/4 games) — Zero-Config

The complete, deployed, empirically verified configuration that gives PS
icons + correct input + touchpad for a ds4linux virtual DS4 under Proton.
Four games verified on one config: Detroit (libScePad — icons+input+touch),
Witcher 3, BG3, Death Stranding (SDL2 — icons+input).

**Nothing lives in the game prefix.** All five components are system-side;
the user's only per-game action is selecting the patched Proton.

## Component map

| # | Component | Location | Change | Why it exists |
|---|---|---|---|---|
| 1 | `ds4ctl` wrapper | `/usr/local/bin` | (existing) | chmod 000 real DualSense hidraw (else double input from the original in Wine), stray-daemon cleanup, lifecycle |
| 2 | ds4linux daemon | `/usr/bin/ds4linux-daemon` | 4 patches (below) | Emulates a *believable* DS4 |
| 3 | `winebus.so` | dist `-B1/.../x86_64-unix/` | V1+V1.1+V1.2 (~20 lines, `bus_udev.c`) | is_gamepad=1 → winexinput stack; Sony strings; version 0x0100 |
| 4 | `winexinput.sys` | dist both arch + **prefix copy** | UTF-16 `&IG_00`↔`&XI_00` swap | Real descriptor on the IG_00 path (the path libScePad opens and the WMI `IG_` gate matches); synthetic goes to XI_00 |
| 5 | `hidclass.sys` | dist both arch | GUID 6C53D5FD→…E1A7 | XI_00 interface registered under a dead GUID → the twin is invisible to **all** consumers (XInput, DInput, SDL) |

## Daemon patches (ds4linux, `daemon/src/`)

1. **Name (E1):** `virtual_device.cpp` UHID_CREATE2 name = `"Wireless Controller"` (not the full HID_NAME concat).
2. **Descriptor (E6):** byte-identical to ViGEmBus `Ds4HidReportDescriptor` (467 B): byte76 LogicalMax 0x7F (Sony-native), no 0xB1/0xB2 tail.
3. **Feature 0xA3 blob:** 49-byte ViGEm reference (`"Aug  3 2013"…`), LE16 version @ +0x23 = **0x3100**. The Detroit gate is `> 0x30FF` — a zeroed blob (daemon default) fails it forever.
4. **Touchpad:** report[33]=0x00 (connection), report[34]=counter++, touch1 @35–38 (`[35]` bit7=inactive/0x7F id, `[36]` X-lo, `[37]` Y-lo4<<4|X-hi4, `[38]` Y-hi), touch2 @39 inactive. Reader in `device_manager.cpp`: ABS_MT_POSITION_X/Y + ABS_MT_TRACKING_ID + BTN_TOUCH/BTN_LEFT → `ds4_state`, **send_report on every touchpad SYN** (don't wait for gamepad-node activity). Also battery report[30]=0x1A, report[12]=0x0B.

Touch layout arbitration: the daemon had a one-byte shift (counter@33, touch@34) vs DS4Windows (`DS4_TOUCHPAD_DATA_OFFSET = 35`) and the Detroit parser (counter@34). The Detroit decompile is ground truth; DS4Windows agrees with it.

## winebus patch (bus_udev.c, after `desc.is_hidraw = TRUE;`)

```c
if (is_dualshock4_gamepad(desc.vid, desc.pid) || is_dualsense_gamepad(desc.vid, desc.pid)) {
    desc.is_gamepad = TRUE;                      // V1
    // V1.1: Windows-correct strings
    // manufacturer ← "Sony Computer Entertainment", product ← "Wireless Controller",
    // serial ← strip ':' chars
    if (!desc.version) desc.version = 0x0100;    // V1.2
}
```

Version note: parentless-UHID has no `PRODUCT=`/bcdDevice in uevent — winebus reads version from the parent's uevent line (bus_udev.c `get_device_subsystem_info`), so daemon-side `uhid_create2.version` never arrives. V1.2 patches it wine-side; hwdb injection does NOT work (raw sysfs read, not udev properties).

## Hex-patch recipes (data-only, version-portable)

**Swap (winexinput.sys):** count UTF-16 `&\0I\0G\0_\0000\0000\0` and `&\0X\0I\0…` — must each be exactly 1. Replace IG→**unique non-zero placeholder** (e.g. AABBCCDD…), XI→IG, placeholder→XI. NEVER use a zero placeholder (matches hundreds of padding windows → corrupt binary). Equal length, assert counts==1 before and after. Apply to dist x86_64 + i386 **and** the prefix's `drivers/winexinput.sys` copy (the one actually loaded).

**GUID kill (hidclass.sys):** find LE-struct GUID `fdd5536c80640f44b618476750c5e1a6` (1 occurrence), flip last byte. Prefix copy does NOT help for this driver — patch the dist.

## Verification without a game (both via winegcc, keep in repo)

- **hidpaths/hidprobe:** SetupDi enum + Attributes + GetCaps + GetFeature(0x12/0xA3). GREEN = `VER=0100`, `InLen=64 FeatLen=64`, `GetFeature(0x12)` returns MAC, `GetFeature(0xA3)` → `ver@23=3100`. Run against the *game's* prefix (winebus instance matters).
- **ditest:** DirectInput8 EnumDevices + XInputGetCapabilities×4. GREEN = 0 dinput devices, XInput[0..3] all ERROR_DEVICE_NOT_CONNECTED.

## Two game-side input models (both served by one config)

- **libScePad games (Sony ports):** WMI `IG_` gate → real-descriptor IG_00 interface → GetFeature 0x12/0xA3/0x02 dance → connect. Needs the full stack.
- **SDL2 games:** winebus SDL backend (host libSDL sees raw /dev/hidrawX) → `sdl_add_device 054c:05c4 is_gamepad 1` → game opens the device directly. gamecontrollerdb knows the VID/PID → PS glyphs. XInputGetState stays 0; the twin is invisible via the dead GUID.

**winebus-SDL ≠ native Windows SDL:** the former never reads interface paths — the "SDL sees IG_ → delegates to XInput" pattern is a native-Windows-SDL behavior. Don't transplant that diagnosis under Proton (it cost a false "impossible" verdict for Witcher 3 that the final config disproved empirically).

## Rollback inventory (deployed machine)

daemon `.touchbak`/`.a3bak`/`.strings-e1bak`/`.pre-e6bak`; winebus `.b1bak`/`.v1bak`/`.v11bak`; winexinput `.swbak` ×3; hidclass `.gdbak` ×3; `system.reg.e5bak`/`.igbak`; INF original at `~/winexinput.inf.prefix.orig`. dinput-key in user.reg was removed as redundant (verified by run).

## Upstream posture

- **V1 (is_gamepad)** — honest bugfix (real DualSense over USB suffers identically); MR-ready for wine-mirror.
- V1.1/V1.2 strings+version, SWAP, GUID — product-only (GE-style distribution channel), NOT upstream material: they fabricate data or trade XInput-only-game compatibility for DS4-first policy. On this config an XInput-only game without SDL fallback loses the pad — that's the accepted scope.
