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

- [ ] **One-command install (the top priority — current install is pain
      even for the author)**. Decision (2026-08-18): curl|bash script
      delivering STATIC binaries from GitHub Releases, NOT source-build
      on the user machine:
      `curl -fsSL https://github.com/NaGGaN-Z/proton-ds/install.sh | bash`
      - GH Actions release pipeline: static gadget-shim + daemon
        (check: only libstdc++/libevdev externals — both static-linkable)
        in an Arch container; sha256-gated download (mechanics proven by
        the v0.1 winebus prebuilt flow)
      - preflight fail-fast: kernel modules (dummy_hcd/libcomposite/
        usb_f_fs), stock GE instance present, running-as-root check —
        human-readable "what to install" messages
      - install (idempotent): binaries to /usr/bin, NOPASSWD sudoers
        drop-in (`/etc/sudoers.d/ds4ctl`, exact path, visudo -c before
        and after), desktop shortcuts (xdg-user-dir DESKTOP, localized)
      - verify-gadget run at the end → "ready, click Start"
      - `--from-source` fallback for -git users / odd Arch variants
      - uninstall.sh symmetric reversal
      - TARGET: Arch family ONLY for v0.2.x (dev platform); other distros
        = issues, by demand (README → Portability has the kernel check)
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

- [ ] Release CI (shared with the one-command install): build static
  binaries on tag push, attach + sha256 to the Release
- [ ] GitHub Actions: stock-vs-patched dedup-drift test case (catches
      wine changes to the Sony allowlist/priorities — the lesson of
      2026-08-18: "stock chain is dead" was wrong for GE11-3)
- [ ] GE-Proton version matrix: gadget path needs NOTHING per-version
      (that's the point) — the matrix is regression testing, not builds
- [ ] Legacy path maintenance: keep v0.1 stack working for old GE;
      deprecation note for new GE (winebus patches harmful on GE11-3+)
- [ ] Test matrix: 2-3 kernels (dummy_hcd availability), BT dongles

## v0.4 — Comfort & adoption

- [ ] Profile engine (prerequisite for GUI value): apply `Profile` in
      the daemon's `translate_events` (button remap, stick
      sensitivity/deadzones) — today the mapping is hardcoded inline and
      `slot.profile` is only used for the lightbar; headless main does
      not start the IPC server, so LoadProfile/SaveProfile have no
      consumer. Once the engine exists, profiles work in BOTH paths for
      free (the daemon is shared); bridge mode must keep the
      game-controlled lightbar (skip profile lightbar there)
- [ ] `proton-ds status` — one-command health check (daemon, gadget,
      bridge, per-instance posture)
- [ ] Tray GUI (thin wrapper over CLI): emulation ON/OFF, Proton status
      — a FULL GUI waits for the profile engine + multi-pad (nothing to
      manage yet beyond two buttons)
- [ ] Troubleshooting guide (PROTON_LOG wizard; known incidents)
- [ ] AUR / COPR packaging

## Backlog / research

- [ ] **BT-bridge conservative profile (contingency — build if the
      freeze survives the BIOS update)**: the solution must not depend
      on users having fresh AGESA. Knobs to try, cheapest first:
      input-report coalescing (fixed-tick forward of the LATEST report
      at ~125 Hz — note: a byte-diff filter is useless for input, the
      IMU noise makes every report "changed" even at rest; the BT stream
      is a flat 250 Hz metronome 24/7 while connected), lazy output
      re-send (diff IS valid here — lightbar/rumble change rarely; min
      16 ms interval), optional `--conservative` flag for ds4ctl.
      Validate on the only affected machine we have (A520M K V2,
      pre-F7a BIOS preserved in the "breaks" state until the profile
      is proven).

- [ ] **Uninstall / stop must clean bluez ghosts**: every stack start
      leaves a pending-auth bond at `00:00:00:00:00:00` (zero-MAC side
      effect, see SOLUTION.md "Ghost bond artifact"); pre-fix eras may
      have left MAC+1 ghosts. `ds4ctl gadget stop` and the uninstall
      path should run, with bluetoothd running and the stack down:
      `bluetoothctl remove 00:00:00:00:00:00` (+ restart bluetooth if
      the remove is refused), and sweep bonds whose MAC matches
      gadget-era serials but not the real pad.
- [ ] **Renaming decision (open, discussed 2026-08-18)**: "proton-ds"
      describes the v0.1 method (patching Proton); the gadget path never
      touches Proton. Candidate: **doppelganger** (repo) + `doppel`
      (CLI, ds4ctl stays as a transition alias); runner-ups: ps-glyphs,
      dualsense-ds4. Do the rename BEFORE wide launch (repo redirect is
      free, but naming after adoption is not).
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
