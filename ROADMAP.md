# Roadmap

## v0.1 — Reproducible install (core milestone)

- [ ] `setup.sh`: apply the winebus patch
  - [ ] build path: clone wine (GE-pinned commit) + GE patches + V1-V1.2 → module-only build
  - [ ] prebuilt path: fetch `winebus.so` from Releases (CI artifact matrix)
- [ ] `setup.sh`: hex-patches (winexinput swap ×3 targets incl. prefix copy,
      hidclass GUID ×2 dist copies) with occurrence-count asserts
- [ ] `setup.sh`: daemon + ds4ctl install from the ds4linux fork
- [ ] `uninstall.sh`: full rollback from `.swbak/.gdbak/.b1bak` backups
- [ ] verify.sh wired into setup as the final gate (ALL GREEN or abort+rollback)
- [ ] Smoke-test on a clean second machine (the 4/4 matrix re-run)

## v0.2 — Version matrix & CI

- [ ] GitHub Actions: build winebus.so for a matrix of GE-Proton versions
- [ ] Auto-detect GE version → pick matching prebuilt (or warn+build)
- [ ] Optional upstream contribution: descriptor-based hidraw gamepad
      classification (a NEW design, not our VID/PID patch — the allowlist
      is product policy, see docs/SOLUTION.md upstream posture). Only if
      someone wants to do the wine-review dance.
- [ ] Test matrix: 2-3 kernels, Steam Deck (read-only /usr caveat!), NixOS/immutable distros notes

## v0.3 — Comfort & adoption

- [ ] `proton-ds status` — one-command health check (daemon, gadget, Proton patches, per-game prefix state)
- [ ] Tray GUI (thin wrapper over CLI): emulation ON/OFF, Proton status, game matrix
- [ ] Troubleshooting guide (log capture wizard: PROTON_LOG flags explained)
- [ ] AUR / COPR packaging

## Backlog / research

- [ ] SenseShock cross-check (daemon-side blobs comparison; why no BT there)
- [ ] hex-patches → source .patch files against winexinput/hidclass
      (survives rebuilds, reviewable)
- [ ] Native DualSense regression on patched Proton (RESOLVED root cause
      2026-08-15): V1 (is_gamepad) lifts the winexinput stack over the real
      DS5 → game sees synthetic IG_03 + GUID-hidden XI_03, and the bare
      `MI_03` interface (which stock serves and DS:DC opens natively) is
      gone → input dead on B1, works on stock. Fix candidate: restrict V1
      to DS4 PIDs (05C4/09CC), leave DualSense on the stock path.
      E2-strings/V1.2 also need the same PID review.
- [ ] E3 gadget topology (f_hid) — the "honest" alternative to wine-side
      strings/version patches; revisit if upstream resistance to V1.1/V1.2

## Done

- [x] Daemon fixes (name/descriptor/0xA3/touchpad) — [ds4linux PR #3](https://github.com/PalashDalsaniya/ds4linux/pull/3)
- [x] Gameless verifiers (hidprobe/ditest/hidpaths + verify.sh)
- [x] Hex-patch recipes documented with safety rules
- [x] 4/4 game matrix verified (Detroit, W3, BG3, DS)
- [x] Zero per-prefix config proven (dinput key removed as redundant)
