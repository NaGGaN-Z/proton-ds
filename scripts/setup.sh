#!/usr/bin/env bash
# proton-ds4emu setup — install the DS4-under-Proton stack.
# Skeleton: detection + verification wired; patch application TODO.
set -euo pipefail

CT_DIR="${CT_DIR:-$HOME/.local/share/Steam/compatibilitytools.d}"

say() { printf '\033[1;36m[proton-ds4]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[proton-ds4] FATAL:\033[0m %s\n' "$*" >&2; exit 1; }

# ── 1. Detect GE-Proton installs ────────────────────────────────────────────
say "scanning $CT_DIR ..."
shopt -s nullglob
targets=("$CT_DIR"/GE-Proton*)
shopt -u nullglob
[ ${#targets[@]} -gt 0 ] || die "no GE-Proton found in $CT_DIR"

for d in "${targets[@]}"; do
    name=$(basename "$d")
    wb="$d/files/lib/wine/x86_64-unix/winebus.so"
    [ -f "$wb" ] || { say "  $name: no winebus.so, skip"; continue; }
    # stock = 110744 B GE build; patched builds differ — detect by marker
    if grep -qa "Sony Computer Entertainment" "$wb" 2>/dev/null; then
        say "  $name: ALREADY PATCHED (strings marker present)"
    else
        say "  $name: stock (candidate for patching)"
    fi
done

# ── 2..4. TODO: winebus build/patch + hex-patches + daemon install ──────────
say "patch application not implemented yet (skeleton)"

# ── 5. Verification (works on an already-patched system) ────────────────────
run_verifier() {
    local exe="$1" pfx="$2"
    [ -x "$exe" ] || { say "  verifier missing: $exe"; return 1; }
    WINEPREFIX="$pfx" "$exe" 2>/dev/null
}

say "verifiers live in verifiers/ (build with winegcc, see README)"
say "done (skeleton)"
