# The Solution Stack (2026-08-15, verified 4/4 games) — Zero-Config

The complete, deployed, empirically verified configuration that gives PS
icons + correct input + touchpad for a ds4linux virtual DS4 under Proton.
Four games verified on one config: Detroit (libScePad — icons+input+touch),
Witcher 3, BG3, Death Stranding (SDL2 — icons+input).

**Nothing lives in the game prefix.** All five components are system-side;
the user's only per-game action is selecting the patched Proton.

## E3: the gadget path (v0.2 Phase 1, 2026-08-18) — STOCK Proton, zero patches

A USB gadget DS4 (054c:09cc, real 507 B descriptor) raised on `dummy_hcd`
via configfs+FunctionFS. The kernel registers it as a real DualShock 4
(`hid-sony: Registered DualShock4 hw_version=0x3100`), wine's stock Sony
allowlist makes hidraw win the dedup — no twins, no IG_/XI_ dance, no
hex-patched drivers. The stack above (B1) stays for the uhid/daemon path
and old GE instances; for GE-Proton11-3+ stock it is obsolete.

| Game | Proton (stock) | Result | Notes |
|---|---|---|---|
| Detroit: Become Human | Proton 9.0-4 | ✅ icons + input | libScePad accepted the gadget: GetFeature 0x12/0xA3 chain green in gameless verify |
| The Witcher 3 | GE-Proton11-3 | ✅ icons + input | winebus created the device from `dummy_hcd.0/.../hidraw10` |

Runtime telemetry (user session ~5.4 min): `reports=14783 ep1_writes=14783`
(100 % delivery), `ep2_reads=276` (games sent output). Touchpad/gyro
in-game checks pending — tracked for wide-test follow-ups.
Gameless criteria + teardown ×3: `verifiers/verify-gadget.sh` (ALL GREEN).
Logs preserved: `steam-1222140-e3-detroit.log`, `steam-292030-e3-w3.log`.

Rumble/lightbar (physically verified 2026-08-18, kernel-referenced):
DualSense USB output = **63-byte** report 0x02; `[1]flag0=0x02`
(HAPTICS_SELECT), `[2]flag1=0x04` (lightbar), `[3]weak [4]strong`,
**`[39]flag2=0x04`** (COMPATIBLE_VIBRATION2 — the v2-pad rumble switch,
invisible in usbmon's 32 B truncation), `[45..47]` RGB; rumble+lightbar
must share ONE report (separate writes erase the motors). Ground truth =
usbmon capture of hid-playstation's own frames + `dualsense_output_worker`.

Identity notes (empirically pinned):
- Gadget serial **must** be the pad MAC +1 (last octet): hid-playstation
  dedups controllers by MAC and refuses a duplicate (`probe failed -17`).
- **Feature 0x12 answers with a ZERO MAC** (daf499f): bluez's sixaxis
  plugin opens the gadget hidraw, reads 0x12, and with a real-looking
  address opens kernel BT pairing sessions — repeated stack cycles
  deadlocked the kernel (silent lockup: scheduler dead, network softirq
  alive; freeze ×3 2026-08-18, journal-proven by
  `sixaxis: setting up new device` seconds before each death). Zero
  address → harmless pending D-Bus auth, no kernel session. Wine gates
  on the transfer succeeding, not the value.
- **Ghost bond artifact (cost of the zero MAC)**: on each FIRST stack
  start (no existing ghost) the pending sixaxis auth pops a KDE
  authorization dialog ("Wireless Controller requests access"). The
  ghost bond directory
  `/var/lib/bluetooth/<adapter>/00:00:00:00:00:00/` is created the
  moment the user clicks Allow — bluez writes the bond (Name=Wireless
  Controller, CablePairing=true, Trusted=true) BEFORE validating the
  address, then logs `Failed to add device 00:00:00:00:00:00: Invalid
  Parameters (0x0d)` — expected signature, the ghost persists anyway.
  Verified live 2026-08-19: enumeration alone creates nothing; the
  bond's mtime == the Allow click. Because the stored bond is Trusted,
  subsequent stack starts should skip the dialog (popup is once per
  machine). Properties: harmless to the kernel (that was the fix),
  survives reboots, stacks as at most ONE entry per machine (constant
  MAC). Pre-fix shims that answered with per-epoch MACs (real MAC,
  MAC+1, …) left one ghost per epoch — distinguishable from the real
  pad's bond only by MAC; check `ls /var/lib/bluetooth/<adapter>/`.
  Cleanup with the stack DOWN:
  `systemctl restart bluetooth && bluetoothctl remove 00:00:00:00:00:00`
  (repeat for any stale MAC+1 ghosts; never touch the real pad's MAC).
  `ds4ctl gadget stop` / uninstall MUST do this automatically — see
  ROADMAP.
- `dummy_udc.0/state` sticks at `configured` after clean unbind (quirk);
  the real unbind marker is the gadget hidraw disappearing + dmesg
  `USB disconnect`.
- Stale registry ghosts (`IG_00…DS4EMU001` from the SenseShock era) are
  inert — the WMI gate reads live PnP, not registry.

### Phase 2 — daemon bridge (BT input, 2026-08-18)

`ds4ctl gadget start --bridge` runs the ds4linux fork daemon in
`--gadget-bridge` mode: it owns the pad (evdev grab, PR#3 translations,
profiles) and streams **final 64-byte DS4 reports** over
`/run/ds4linux-bridge.sock` (binary framing per `gadget-bridge-spec.md`:
`[u32 LE len][u8 type][data]`, latest-wins, reconnect 250ms→2s); the
shim forwards to ep1 and relays host output back. Verified: **Detroit
PS icons + input over a Bluetooth DualSense on stock Proton 9.0-4** —
the capability SenseShock lacks (their libusb input is USB-only).

Output encoding is transport-aware (kernel hid-playstation.c referenced,
usbmon-verified): USB = 63-byte report 0x02; **BT = report 0x31, 78 B,
seq<<4 @1, tag 0x10 @2, CRC32(seed 0xA2 over bytes 0..73) @74 LE**.
Rumble on vibration_v2 pads needs `valid_flag2@39=0x04`; rumble+lightbar
ride ONE combined report from persistent state. The legacy daemon's
48-byte writes were never physically correct (partial lightbar, zero
rumble) — fixed in the fork alongside the bridge work.

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

**None of this is upstream-ready as-is.** Honest assessment:

- **V1 (is_gamepad)** — NOT a clean bugfix, despite appearances. The real
  gap is that the hidraw backend has *no gamepad classification for any
  device* (an Xbox pad on hidraw hits the same wall). Our patch papers
  over that gap with a Sony VID/PID allowlist — exactly the pattern wine
  review rejects ("don't hardcode vendors; classify from the descriptor
  like the evdev path does"). An upstream-quality fix would implement
  descriptor-based classification in the hidraw path and drop the
  allowlist. Our V1 also exists to lift the winexinput stack on purpose
  (product policy), which is not what upstream wants hidraw
  classification to mean.
- **V1.1/V1.2 strings+version** — data fabrication (pretend USB parent
  topology); not upstream material on principle.
- **SWAP, GUID** — product policy, trades XInput-only-game compatibility
  for a DS-first setup; not upstream material. On this config an
  XInput-only game without SDL fallback loses the pad — accepted scope.

A possible future upstream contribution is a *descriptor-based* hidraw
classification (separate design work, not this patch); until then this
repo ships everything itself, GE-Proton-style.
