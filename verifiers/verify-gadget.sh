#!/usr/bin/env bash
# verify-gadget.sh — gameless verification of the E3 gadget path on a STOCK
# Proton instance. Zero wine patches assumed; the criteria below are the
# STOCK-path norm (empirically confirmed 2026-08-18 against live winebus).
#
# Usage:
#   verify-gadget.sh <GE-dir>                    # verify phase (gadget must be UP)
#   verify-gadget.sh --teardown-only             # assert configfs/ffs/UDC/pad cleanliness
#   verify-gadget.sh --full <GE-dir> [N]         # N× start→verify→stop cycles (needs sudo -n)
#
# Stock criteria (why they differ from verify.sh of the uhid/B1 path):
#   - hidpaths: exactly ONE 054C:09CC with MI_00; NO IG_/XI_ twins — on stock,
#     the Sony allowlist makes hidraw WIN the dedup race, twins never form.
#   - hidprobe: VER=0100 (bcdDevice), InLen=64, GetFeature(0x12) OK, ver@23=3100.
#   - ditest: XInput[0..3] caps=0000048F is the NORM here (stock hid-based
#     XInput); ERROR_DEVICE_NOT_CONNECTED was a B1-only expectation.
#     dinput listing of the device is expected (no GUID patch) — informational.
#
# NB: run the verify phase from a USER session (e.g. the desktop user),
# not as root — under a root environment hidprobe historically prints nothing.

set -uo pipefail
VER_DIR="$(cd "$(dirname "$0")" && pwd)"

say()  { printf '\033[1;36m[verify-gadget]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[verify-gadget] FAIL:\033[0m %s\n' "$*" >&2; exit 1; }

GADGET_SHIM="${PDS_GADGET_SHIM:-/usr/bin/gadget-shim}"
DS4CTL="${PDS_DS4CTL:-/usr/local/bin/ds4ctl}"
CYCLES=3

cmd_verify() {
    local GE="$1"
    local W="$GE/files/bin/wine"
    [ -x "$W" ] || fail "wine binary not found: $W (pass the STOCK GE dir)"
    [ -x "$VER_DIR/hidpaths.exe" ] || fail "verifiers not built: run $VER_DIR/build.sh"

    # The gadget must be up and enumerated before we look.
    local gad_hid=""
    gad_hid="$(grep -l "0003:0000054C:000009CC" /sys/class/hidraw/*/device/uevent 2>/dev/null | head -1)"
    [ -n "$gad_hid" ] || fail "gadget 054C:09CC hidraw not found — is the gadget started?"

    # Fresh prefix + PATH pinning of the stock instance's wine.
    local PFX
    PFX="$(mktemp -d /tmp/pds-vfy-XXXXXX)"
    export WINEPREFIX="$PFX"
    export WINEDEBUG=-all
    export PATH="$GE/files/bin:$PATH"
    # set -u: append safely even when LD_LIBRARY_PATH is unset
    export LD_LIBRARY_PATH="$GE/files/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    say "── hidpaths (one 09CC, MI_00, no twins) ──"
    local out
    out="$("$VER_DIR/hidpaths.exe" 2>/dev/null || true)"
    echo "$out"
    local n09cc
    n09cc="$(grep -c "054C&PID_09CC\|vid_054c&pid_09cc" <<<"$(echo "$out" | tr 'A-Z' 'a-z')" || true)"
    [ "$(grep -ci "054c.*09cc" <<<"$out")" -ge 1 ] || fail "no 054C:09CC in hidpaths output"
    echo "$out" | grep -qi "mi_00" || fail "09CC path lacks MI_00 (interface node shape wrong)"
    if echo "$out" | grep -q "IG_\|XI_"; then
        fail "IG_/XI_ twins present — this is a PATCHED instance, not stock (or V1.3 active)"
    fi
    local cnt
    cnt="$(echo "$out" | grep -c "VID=054C PID=09CC" || true)"
    [ "$cnt" -eq 1 ] || fail "expected exactly one 054C:09CC entry, got $cnt (twins!)"

    say "── hidprobe (the libScePad gate chain) ──"
    out="$("$VER_DIR/hidprobe.exe" 2>/dev/null || true)"
    echo "$out"
    if [ -z "$out" ]; then
        fail "hidprobe printed NOTHING — likely run from root session; run as the desktop user"
    fi
    echo "$out" | grep -q "VER=0100"    || fail "Attributes version != 0100"
    echo "$out" | grep -q "InLen=64"    || fail "input report length != 64"
    echo "$out" | grep -q "0x12) OK"    || fail "GetFeature(0x12) MAC failed"
    echo "$out" | grep -q "ver@23=3100" || fail "0xA3 version != 0x3100 (Detroit gate)"

    say "── ditest (stock XInput norm — informational dinput) ──"
    out="$("$VER_DIR/ditest.exe" 2>/dev/null || true)"
    echo "$out"
    if echo "$out" | grep -q "XInput\[0\] caps=0000048F"; then
        say "XInput[0] caps=0000048F — stock norm (OK)"
    elif echo "$out" | grep -q "ERROR_DEVICE_NOT_CONNECTED"; then
        fail "caps=ERROR_DEVICE_NOT_CONNECTED — that is the B1 expectation, NOT stock; suspicious winebus"
    else
        say "NOTE: ditest output unexpected — inspect manually"
    fi

    rm -rf "$PFX"
    say "PHASE GREEN"
}

cmd_teardown_asserts() {
    local bad=0
    say "── teardown assertions ──"
    local n
    n="$(ls -A /sys/kernel/config/usb_gadget/ 2>/dev/null | wc -l)"
    [ "$n" -eq 0 ] || { printf 'configfs not empty\n' >&2; bad=1; }
    [ ! -e /dev/ffs-pds4 ] || { printf '/dev/ffs-pds4 still exists\n' >&2; bad=1; }
    # NB: /sys/class/udc/dummy_udc.0 remains listed while the module is
    # loaded, and its `state` sticks at "configured" after a clean unbind
    # (dummy_hcd quirk; dmesg shows the real "USB disconnect"). The direct
    # marker of a free UDC is the absence of the gadget's hidraw node.
    if grep -q "0003:0000054C:000009CC" /sys/class/hidraw/*/device/uevent 2>/dev/null; then
        printf 'gadget hidraw 054C:09CC still present — device not unbound\n' >&2
        bad=1
    fi
    if [ -f /run/ds4ctl-hidden-hidraw ]; then
        printf 'hidden-hidraw list still present (real pad still hidden)\n' >&2
        bad=1
    fi
    local real
    real="$(grep -l "0003:0000054C:00000CE6" /sys/class/hidraw/*/device/uevent 2>/dev/null | head -1)"
    if [ -n "$real" ]; then
        local node="/dev/$(basename "$(dirname "$(dirname "$real")")")"
        [ -r "$node" ] && [ -w "$node" ] || { printf 'real pad %s not accessible\n' "$node" >&2; bad=1; }
    fi
    [ "$bad" -eq 0 ] || fail "teardown assertions failed"
    say "TEARDOWN CLEAN"
}

cmd_full() {
    local GE="$1" cycles="${2:-$CYCLES}" i
    command -v sudo >/dev/null || fail "sudo not available"
    for ((i = 1; i <= cycles; i++)); do
        say "═══ cycle $i/$cycles ═══"
        sudo -n "$DS4CTL" gadget start || fail "cycle $i: ds4ctl gadget start failed (sudo -n passwordless?)"
        cmd_verify "$GE"
        sudo -n "$DS4CTL" gadget stop || fail "cycle $i: ds4ctl gadget stop failed"
        cmd_teardown_asserts
    done
    say "ALL CYCLES GREEN (×$cycles)"
}

case "${1:-}" in
    --teardown-only) cmd_teardown_asserts ;;
    --full)          shift; cmd_full "${1:-}" "${2:-$CYCLES}" ;;
    -h|--help|help)
        sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)               [ -n "${1:-}" ] || { sed -n '2,16p' "$0"; exit 64; }
                     cmd_verify "$1" ;;
esac
