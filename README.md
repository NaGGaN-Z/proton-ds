# proton-ds — DualShock family under Proton, done right

PS glyphs + correct native input + touchpad for a DualSense→DS4 virtual
gamepad under Proton/Wine. Verified 4/4:

| Game | Input layer | Result |
|---|---|---|
| Detroit: Become Human | libScePad (strict probe) | ✅ icons + input + touchpad |
| The Witcher 3 | SDL2 | ✅ icons + input |
| Baldur's Gate 3 | SDL2 | ✅ icons + input |
| DEATH STRANDING DC | SDL2/Decima | ✅ icons + input |
| DEATH STRANDING DC (native DualSense, no daemon) | MI_xx passthrough | ✅ icons + input |

Device coverage: DS4 family via emulation (DualShock 4 v1/v2, and any
source controller the daemon can read — DualSense→DS4 is the verified
path), native DualSense passthrough. DS3/DS2 are out of scope (different
protocols, not HID-DS4-style).

Zero per-game configuration: everything installs system-side; the only
user action is selecting the patched Proton instance for a game.

## How it works (the five components)

1. **ds4ctl wrapper** — lifecycle + hides the real DualSense hidraw
   (prevents double input).
2. **ds4linux daemon (fork)** — a *believable* DS4: Windows-correct name,
   ViGEm-identical 467-byte descriptor, firmware-info feature 0xA3 blob
   (version 0x3100 @ +0x23 — strict games gate on > 0x30FF), battery
   bytes, full touchpad translation. Source: [NaGGaN-Z/ds4linux][fork],
   branch `proton-compat` ([PR #3][pr]).
3. **winebus.so patch** (V1/V1.1/V1.2/V1.3 in `bus_udev.c` + a
   `main.c`/`bus_sdl.c` tweak) — hidraw gamepad classification for DS4
   PIDs only + Windows-correct strings + version 0x0100; DualSense stays
   on the native path.
4. **winexinput.sys IG_00↔XI_00 swap** (data hex-patch) — puts the REAL
   descriptor on the IG_00 interface path (what libScePad opens and the
   WMI `IG_` gate matches).
5. **hidclass.sys GUID patch** (data hex-patch) — registers the synthetic
   twin interface under a dead GUID → invisible to XInput/DInput/SDL at
   once → no double input, no per-game registry hacks.

Full engineering story (detection-chain decode, why each piece exists,
rollback inventory): `docs/SOLUTION.md`.

## Install

```bash
git clone https://github.com/NaGGaN-Z/proton-ds
cd proton-ds
./scripts/setup.sh
```

`setup.sh` works on the **instance model**: it lists every Proton instance
in `~/.local/share/Steam/compatibilitytools.d/`, you pick one, it makes a
`<name>-DS` copy and patches **only the copy** (the stock instance is
never touched; prefixes are never touched — wineboot propagates driver
copies from the dist of the instance a game runs on):

1. Instance scan → selection → `cp -a` to `<name>-DS` (disk-space check,
   safe-abort cleanup on any failure)
2. **winebus**: prebuilt `.so` from Releases (sha256-gated) when your
   instance has one; otherwise it offers a source build
   (wine@9578fa3 + GE patches 0018/0036 + V1.3 patch; needs gcc,
   autoconf, bison, flex; module-only, ~10-30 min)
3. **Driver hex-patches** in the copy: winexinput IG_00↔XI_00 swap ×2,
   hidclass GUID ×2 — occurrence-count asserts, backups next to each file,
   idempotent (already-patched → skip)
4. **Daemon + ds4ctl** (sudo): builds the [fork][fork] daemon, one-time
   `.pdsbak` backups of any existing binaries, installs. The daemon is
   NOT auto-started — lifecycle is yours (`sudo ds4ctl start|stop|status`)
5. **Verify gate**: gameless verifiers in a throwaway prefix — ALL GREEN
   (needs a connected DualSense + running daemon; without them setup
   degrades to `verified=false` instead of failing). FAIL → automatic
   rollback of the -DS copy.

Flags: `--instance NAME`, `--dry-run` (full plan, zero changes),
`--force`, `--with-build` (prefer the source build), `--skip-verify`.

## Uninstall

```bash
./scripts/uninstall.sh            # pick a -DS instance, removes it
./scripts/uninstall.sh --all --system --purge-cache
```

Manifest-driven (`proton-ds.json` inside each -DS instance). `--system`
stops the daemon and restores daemon/ds4ctl from the `.pdsbak` backups
(or removes them when absent). Note: uninstalling does not touch a
running session — stop the daemon before launching games on the stock
instance.

## Layout

```
scripts/    setup.sh / uninstall.sh (install pipeline), hexpatch.py (hex engine), ds4ctl
patches/    winebus source patches (v13 + ge/); hex-patch recipes (winexinput swap, hidclass GUID)
verifiers/  hidpaths / hidprobe / ditest (winegcc tools, no game needed)
tests/      golden hashes + stock fixtures (hexpatch self-test)
docs/       SOLUTION.md — the decoded detection chain + full stack rationale
```

## Scope & upstream posture

- **Nothing here is upstream-ready as-is.** The `is_gamepad` patch looks
  like a bugfix but hardcodes a Sony VID/PID allowlist over a broader gap
  (hidraw has no gamepad classification for *any* device); an honest
  upstream fix would classify from the HID descriptor instead. This repo
  ships everything itself, GE-Proton-style.
- The swap/GUID patches are product policy (DS-first): on this setup an
  XInput-only game without an SDL fallback loses the pad. Use a stock
  Proton instance for such games.
- Stock Proton + daemon only = pad not visible (empirically verified,
  see PR #3 discussion).
- **Native DualSense works on the -DS instance** (V1.3 PID-scoping): the
  winebus patch lifts the winexinput stack only over DS4-family PIDs
  (05C4/09CC/0BA0); a real DualSense stays on the stock passthrough path.
  Verified: DS:DC native DualSense + Detroit DS4 emulation on the same
  patched instance — the -DS instance is universal.

## Credits

- [ViGEmBus][vigem] — reference DS4 descriptor and firmware blobs (MIT).
- [DS4Windows][ds4w] — touchpad layout cross-check.
- [ds4linux][ds4linux] — the base emulator this fork improves.

Co-developed with GLM-5.2/LLM assistance.

[fork]: https://github.com/NaGGaN-Z/ds4linux
[pr]: https://github.com/PalashDalsaniya/ds4linux/pull/3
[vigem]: https://github.com/nefarius/ViGEmBus
[ds4w]: https://github.com/CircumSpector/DS4Windows
[ds4linux]: https://github.com/PalashDalsaniya/ds4linux
