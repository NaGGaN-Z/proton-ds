# proton-ds4emu — DS4 emulation for Proton with PS icons

PS glyphs + correct native input + touchpad for a DualSense→DS4 virtual
gamepad under Proton/Wine. Verified 4/4:

| Game | Input layer | Result |
|---|---|---|
| Detroit: Become Human | libScePad (strict probe) | ✅ icons + input + touchpad |
| The Witcher 3 | SDL2 | ✅ icons + input |
| Baldur's Gate 3 | SDL2 | ✅ icons |
| DEATH STRANDING DC | SDL2/Decima | ✅ icons + input |

Zero per-game configuration: everything installs system-side; the only
user action is selecting the patched Proton instance for a game.

> Local dev note: this directory is `proton-ds4emu`, the GitHub repo is
> `proton-ds4`. Same thing.

## How it works (the five components)

1. **ds4ctl wrapper** — lifecycle + hides the real DualSense hidraw
   (prevents double input).
2. **ds4linux daemon (fork)** — a *believable* DS4: Windows-correct name,
   ViGEm-identical 467-byte descriptor, firmware-info feature 0xA3 blob
   (version 0x3100 @ +0x23 — strict games gate on > 0x30FF), battery
   bytes, full touchpad translation. Source: [NaGGaN-Z/ds4linux][fork],
   branch `proton-compat` ([PR #3][pr]).
3. **winebus.so patch** (V1/V1.1/V1.2, ~20 lines in `bus_udev.c`) —
   hidraw gamepad classification for Sony VID/PID + Windows-correct
   strings + version 0x0100.
4. **winexinput.sys IG_00↔XI_00 swap** (data hex-patch) — puts the REAL
   descriptor on the IG_00 interface path (what libScePad opens and the
   WMI `IG_` gate matches).
5. **hidclass.sys GUID patch** (data hex-patch) — registers the synthetic
   twin interface under a dead GUID → invisible to XInput/DInput/SDL at
   once → no double input, no per-game registry hacks.

Full engineering story (detection-chain decode, why each piece exists,
rollback inventory): `docs/SOLUTION.md`.

## Install (skeleton state — the script is being filled in)

```bash
git clone https://github.com/NaGGaN-Z/proton-ds4emu   # TODO: repo name
cd proton-ds4emu
sudo ./scripts/setup.sh          # detect GE → patch → verify
```

`setup.sh` pipeline (current skeleton implements detection + verification;
patch application lands next):
1. Detect installed GE-Proton versions under `~/.local/share/Steam/compatibilitytools.d/`
2. Build or fetch prebuilt patched `winebus.so` for the target version
3. Apply the two data hex-patches (with occurrence-count verification)
4. Install the daemon + `ds4ctl` from the [fork][fork]
5. Run the gameless verifiers (`verifiers/`) — all green = done

## Layout

```
scripts/    setup.sh (install), uninstall.sh
patches/    winebus source patch; hex-patch recipes (winexinput swap, hidclass GUID)
verifiers/  hidpaths / hidprobe / ditest (winegcc tools, no game needed)
docs/       SOLUTION.md — the decoded detection chain + full stack rationale
```

## Scope & upstream posture

- `is_gamepad` winebus fix is a genuine upstream bugfix candidate
  (real DualSense over USB suffers identically on stock winebus).
- The swap/GUID patches are product policy (DS4-first): on this setup an
  XInput-only game without an SDL fallback loses the pad. Use a stock
  Proton instance for such games.
- Stock Proton + daemon only = pad not visible (empirically verified,
  see PR #3 discussion).

## Credits

- [ViGEmBus][vigem] — reference DS4 descriptor and firmware blobs (MIT).
- [DS4Windows][ds4w] — touchpad layout cross-check.
- [ds4linux][ds4linux] — the base emulator this fork improves.

[fork]: https://github.com/NaGGaN-Z/ds4linux
[pr]: https://github.com/PalashDalsaniya/ds4linux/pull/3
[vigem]: https://github.com/nefarius/ViGEmBus
[ds4w]: https://github.com/CircumSpector/DS4Windows
[ds4linux]: https://github.com/PalashDalsaniya/ds4linux
