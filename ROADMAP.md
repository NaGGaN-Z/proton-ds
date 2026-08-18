# Roadmap

## v0.2 — E3 gadget path (primary) — shipped 2026-08-18

- [x] Gadget-shim: configfs+FunctionFS lifecycle, ordered teardown,
      orphan cleanup, stale-mount guard (T2)
- [x] HID core: ep0 SETUP answers (507B real / 467B ViGEm descriptor,
      0x02/0x12/0x81/0xA3), ep1 writer (pre-ENABLE backoff), ep2 reader
      (T3) — hid-sony registers the gadget as DS4 hw_version=0x3100
- [x] Standalone input: DualSense evdev → DS4 reports, transport-agnostic
      (T4); output: DS4 0x05 → DS5 63B report → rumble/lightbar (T5)
- [x] ds4ctl gadget start|stop|status, pad MAC+1 serial, real-pad hiding,
      patched-winebus warning (T6)
- [x] verify-gadget.sh: stock-path criteria + teardown asserts + Nx
      cycles — ALL GREEN ×3 (T7)
- [x] Game matrix on stock: Detroit (Proton 9.0-4) + W3 (GE11-3), icons +
      input (T8); rumble physically verified end-to-end
- [x] Daemon bridge mode: --gadget-bridge (no virtual devices, IPC
      stream per docs/gadget-bridge-spec.md), kernel-referenced DS5
      output encoding incl. BT (0x31+CRC32) (T9)
- [x] BT run: Detroit icons+input over a Bluetooth pad on stock Proton —
      the capability SenseShock lacks (T10)
- [x] Freeze incident solved: zero-MAC 0x12 answer vs bluez sixaxis
      pairing (daf499f), stress-verified ×3

## v0.2.x — hardening (next)

- [ ] setup.sh gadget mode: build+install gadget-shim, daemon fork;
      `ds4ctl gadget` as the documented entrypoint
- [ ] Wide game matrix on the gadget path (BG3, DS:DC-emulated, more
      Sony ports); gyro/touchpad in-game checks (T8 leftovers)
- [ ] Multi-pad story: bridge protocol is single-client; define behavior
      (error out / round-robin) and document
- [ ] Steam Deck: read-only /usr constraint (gadget needs writable
      storage for binaries + kernel modules — likely out of scope;
      README → Portability has the one-command check; run it on-device
      for a definitive verdict)
- [ ] Bazzite verify: kernel modules expected present (Fedora family);
      check udev/uaccess rules for hidraw; document result
- [ ] systemd units for stack lifecycle (optional; manual ds4ctl works)

## v0.3 — Version matrix & CI

- [ ] GitHub Actions: stock-vs-patched dedup-drift test case (catches
      wine changes to the Sony allowlist/priorities — the lesson of
      2026-08-18: "stock chain is dead" was wrong for GE11-3)
- [ ] GE-Proton version matrix: gadget path needs NOTHING per-version
      (that's the point) — the matrix is regression testing, not builds
- [ ] Legacy path maintenance: keep v0.1 stack working for old GE;
      deprecation note for new GE (winebus patches harmful on GE11-3+)
- [ ] Test matrix: 2-3 kernels (dummy_hcd availability), BT dongles

## v0.4 — Comfort & adoption

- [ ] `proton-ds status` — one-command health check (daemon, gadget,
      bridge, per-instance posture)
- [ ] Tray GUI (thin wrapper over CLI): emulation ON/OFF, Proton status
- [ ] Troubleshooting guide (PROTON_LOG wizard; known incidents)
- [ ] AUR / COPR packaging

## Backlog / research

- [ ] Profile engine: gadget topology as a generic "virtual USB HID
      device" framework (identity profiles + input sources) — the E3
      shim is one profile (DS4); others (DS3, Switch Pro?) need
      descriptor + feature-blob harvesting first
- [ ] hex-patches → source .patch files against winexinput/hidclass
      (legacy path; survives rebuilds, reviewable)
- [ ] Upstream conversation: is there ANY acceptable upstream shape for
      hidraw gamepad classification? (descriptor-based, no allowlist —
      see docs/SOLUTION.md upstream posture)

## Done

- [x] v0.1 install pipeline (setup.sh/uninstall.sh, hexpatch engine,
      golden-hash self-test) — E2E green on the reference machine
      2026-08-17; game matrix re-run pending (now covered by the gadget
      path for stock GE11-3+)
- [x] Native DualSense regression on patched Proton — RESOLVED 2026-08-15
      (V1.3 PID scoping)
- [x] Daemon fixes (name/descriptor/0xA3/touchpad) — [ds4linux PR #3][pr3]
- [x] Gameless verifiers (hidprobe/ditest/hidpaths + verify.sh)
- [x] 4/4 game matrix on the legacy path (Detroit, W3, BG3, DS)
- [x] Zero per-prefix config proven
- [x] E3 pivot research 2026-08-18: gadget+stock ✓ (Detroit+W3), gadget+
      B1 ✗ (V1.3 spoils 09CC), uhid+stock ✗ (identity-poor) — see
      docs/SOLUTION.md for the full empirical matrix

[pr3]: https://github.com/PalashDalsaniya/ds4linux/pull/3
