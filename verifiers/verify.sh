#!/usr/bin/env bash
# Verify a patched Proton setup WITHOUT running a game.
# Usage: ./verify.sh /path/to/GE-ProtonXXX [/path/to/prefix]
#   Prefix arg optional — winebus behavior differs per-prefix (which dist
#   instance serves it), pass the game prefix for the real check.
set -euo pipefail
GE="$1"; PFX="${2:-}"
W="$GE/files/bin/wine"
say() { printf '\033[1;36m[verify]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[verify] FAIL:\033[0m %s\n' "$*" >&2; exit 1; }

[ -x "$W" ] || fail "wine binary not found: $W"
cd "$(dirname "$0")"
[ -x hidpaths.exe ] || { say "building verifiers..."; ./build.sh; }

export WINEDEBUG=-all
[ -n "$PFX" ] && export WINEPREFIX="$PFX"

say "── hidprobe (the libScePad gate chain) ──"
out=$(./hidprobe.exe 2>/dev/null || true)
echo "$out"
echo "$out" | grep -q "VER=0100"      || fail "Attributes version != 0100 (winebus V1.2 not active for this prefix?)"
echo "$out" | grep -q "InLen=64"      || fail "descriptor is synthetic (InLen=15) — winexinput swap not active"
echo "$out" | grep -q "0x12) OK"      || fail "GetFeature(0x12) MAC failed"
echo "$out" | grep -q "ver@23=3100"   || fail "0xA3 blob version != 0x3100 (daemon not patched/restarted?)"

say "── ditest (twin visibility) ──"
out=$(./ditest.exe 2>/dev/null || true)
echo "$out"
echo "$out" | grep -q "XInput\[0\] caps=0000048F" || fail "XInput twin is VISIBLE (hidclass GUID patch not active)"

say "ALL GREEN"
