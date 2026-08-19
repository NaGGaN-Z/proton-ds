# proton-ds — DualShock family under Proton, done right

PS glyphs + correct native input + touchpad for a DualSense→DS4 virtual
gamepad under Proton/Wine. Two paths, one toolkit:

## E3 — gadget path (v0.2, primary): STOCK Proton, zero wine patches

A **real DS4 raised as a USB gadget** (configfs + FunctionFS on
`dummy_hcd`): the kernel registers it as a genuine DualShock 4
(`hid-sony: Registered DualShock4 hw_version=0x3100`), and wine's stock
Sony allowlist does the rest — no patched drivers, no IG_/XI_ twins, no
per-prefix anything. Works on stock GE-Proton11-3+ and stock Proton 9.x.

| Game | Proton (stock) | Transport | Result |
|---|---|---|---|
| Detroit: Become Human | Proton 9.0-4 | USB / BT | ✅ icons + input |
| The Witcher 3 | GE-Proton11-3 | USB | ✅ icons + input |

```bash
sudo ds4ctl gadget start            # standalone: shim reads the pad (USB)
sudo ds4ctl gadget start --bridge   # daemon owns the pad (USB or BT),
                                    # streams over /run/ds4linux-bridge.sock
ds4ctl gadget status
sudo ds4ctl gadget stop             # real pad restored
```

Requirements: kernel with `dummy_hcd`/`libcomposite`/`usb_f_fs` modules,
root for start/stop. Use a **stock** Proton instance for games; a patched
winebus (B1/V1.3) actively breaks 09CC (`ds4ctl` warns when it detects
one).

Gameless verification: `verifiers/verify-gadget.sh <stock-GE-dir>` —
hidpaths/hidprobe/ditest criteria for the stock path + teardown asserts,
with a `--full` Nx-cycle mode.

## Legacy — uhid path (v0.1): five-component stack for old instances

For GE versions before 11-3 (and as the fallback): patched daemon +
winebus V1.3 + winexinput IG↔XI swap + hidclass GUID kill. 4/4 verified
2026-08-15 (Detroit, W3, BG3, DS:DC). Full story: `docs/SOLUTION.md`.

```bash
./scripts/setup.sh        # instance model: patches a -DS COPY of Proton
./scripts/uninstall.sh
```

## Architecture (E3)

```
DualSense (USB or BT)
   │ evdev (daemon, PR#3 translations)        or  evdev (shim, standalone)
   ▼                                                  ▼
ds4linux-daemon --gadget-bridge ──64B DS4 reports──► gadget-shim
   ▲   (unix socket, latest-wins)                      │ ep1 IN
   │                                            configfs gadget 054c:09cc
   │ rumble/lightbar (63B DS5 output,                 │ on dummy_hcd
   │ BT: report 0x31+CRC32)                           ▼
   └──────────────── ep2 OUT ◄──── hid-sony ◄── real USB enumeration
                                                        ▼
                                     wine (STOCK GE11-3+): hidraw wins
                                     the dedup — no twins, PS glyphs
```

Design notes (hard-won, see `docs/SOLUTION.md` → E3):
- gadget serial = pad MAC **+1** (hid-playstation dedups controllers by
  MAC; a duplicate fails probe with -17)
- feature 0x12 answers with a **zero MAC**: bluez's sixaxis plugin reads
  it on real-looking addresses and opens kernel BT pairing sessions —
  repeated stack cycles can deadlock the kernel (fixed, stress-verified).
  Known side effect: the FIRST stack start pops a KDE auth dialog for
  "Wireless Controller"; clicking Allow leaves one bluez bond at
  `00:00:00:00:00:00` (harmless pending-auth ghost, once per machine —
  later starts skip the dialog). Manual cleanup (stack down):
  `systemctl restart bluetooth && bluetoothctl remove 00:00:00:00:00:00`
- DualSense output report is **63 bytes** (USB) — 48-byte writes light
  the bar partially and never rumble; rumble needs `valid_flag2@39=0x04`
  (vibration_v2 pads) and ONE combined report per effect update

## Known issue: Bluetooth bridge mode can hard-freeze the machine (UNDER INVESTIGATION)

The BT bridge path (physical pad over Bluetooth + gadget) is fully
functional — input, rumble, lightbar all work (BT output encoding
fixed & hardware-verified 2026-08-19). BUT on one test machine the
whole system could hard-freeze with the stack up: 8 freezes over 2
days, all with the stack running (idle, load, or chat — presence, not
activity, correlates). Forensics (live netconsole, armed NMI+soft
watchdogs, panic=10, heartbeat) show the death is BELOW the software
layer: kernel printk, SysRq, NMI and watchdogs all stop in the same
instant with no panic — consistent with SMM/firmware on that board
(stock clocks, vendor BIOS from 2024-02). Whether it is board-specific
or reproducible elsewhere is untested. USB-cable sessions have not
shown a single freeze. Status: open — A/B testing without the stack,
BIOS update pending on the affected machine.
**Until resolved: prefer the USB path for the pad; treat BT mode as
experimental.**

## Anti-pattern: dual-mode (both real pad and gadget visible)

Don't hide-and-seek: if the real DualSense and the gadget DS4 are both
visible, Sony-port games get **dual input**. The stack is a mode, not a
mix: emulation ON (`gadget start`, real pad hidden) or OFF (`gadget
stop`, real pad passes through untouched — use this for native-DS5 games
like DS:DC). `ds4ctl` handles the hiding/restoring automatically.

## Portability

The gadget path needs three kernel modules and a writable spot for the
binaries. One-command compatibility check on any distro:

```bash
modprobe dummy_hcd libcomposite usb_f_fs 2>/dev/null; ls /sys/class/udc
```

`dummy_udc.0` (or any UDC) listed → the path can work (root required for
start/stop, as always).

- **Steam Deck / SteamOS**: unlikely — the Valve kernel ships without
  `dummy_hcd` (no UDC to bind the gadget to; the Deck's real OTG
  controller is not a loopback substitute), and the read-only `/usr`
  adds friction. Untested — run the check above to know for sure.
- **Bazzite / Fedora Atomic**: likely works — Fedora kernels build the
  needed modules, `/home` is writable for the binaries. Untested; the
  udev/uaccess rules for hidraw access may need attention.
- **Conventional distros** (Arch, CachyOS, mainstream Fedora/Ubuntu):
  expected to work — this is the verified configuration family.

## Layout

```
gadget/     gadget-shim (lifecycle, HID core, evdev input, bridge client)
scripts/    ds4ctl (all modes) / setup.sh / uninstall.sh / hexpatch.py
verifiers/  hidpaths / hidprobe / ditest / verify.sh / verify-gadget.sh
patches/    winebus source patches (legacy path)
tests/      golden hashes + stock fixtures (hexpatch self-test)
docs/       SOLUTION.md (both paths' engineering story), gadget-bridge-spec.md
```

## Credits

- [ViGEmBus][vigem] — reference DS4 descriptor and firmware blobs (MIT).
- [DS4Windows][ds4w] — touchpad layout cross-check.
- [ds4linux][ds4linux] — the base emulator this fork improves.
- [SenseShock][senseshock] — proof that the gadget topology works on
  stock Proton (their USB-only libusb approach inspired the E3 pivot;
  our input/BT/rumble layers are our own).

Co-developed with GLM-5.2/LLM assistance.

[fork]: https://github.com/NaGGaN-Z/ds4linux
[pr]: https://github.com/PalashDalsaniya/ds4linux/pull/3
[vigem]: https://github.com/nefarius/ViGEmBus
[ds4w]: https://github.com/CircumSpector/DS4Windows
[ds4linux]: https://github.com/PalashDalsaniya/ds4linux
[senseshock]: https://github.com/muhammad23012009/senseshock
