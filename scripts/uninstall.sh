#!/usr/bin/env bash
# proton-ds uninstall — remove -DS instance(s) and system components.
# Manifest-driven: finds */proton-ds.json in compatibilitytools.d.
set -euo pipefail

VERSION="0.1.0-dev"
CT_DIR="${CT_DIR:-$HOME/.local/share/Steam/compatibilitytools.d}"
CACHE_DIR="${PROTON_DS_CACHE:-$HOME/.cache/proton-ds}"
MANIFEST_NAME="proton-ds.json"
DS_SUFFIX="-DS"

DRY_RUN=0
ALL=0
PURGE_CACHE=0
SYS=0
OPT_NAME=""

say() { printf '\033[1;36m[proton-ds]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[proton-ds WARN]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[proton-ds FATAL]\033[0m %s\n' "$*" >&2; exit 1; }
step() { say "==> $*"; }

usage() {
    cat <<EOF
proton-ds uninstall $VERSION

Usage: uninstall.sh [NAME] [--all] [--system] [--purge-cache] [--dry-run]

  NAME            remove the instance NAME (from the manifest list)
  --all           remove ALL proton-ds instances found
  --system        also remove the system components: daemon and ds4ctl
                  (restores .pdsbak backups if present, else removes)
  --purge-cache   also delete $CACHE_DIR (source trees, build dirs,
                  downloaded prebuilts)
  --dry-run       print the action plan, make zero changes

Without --system the daemon and ds4ctl stay installed (other -DS
instances may still need them; removal is cheap and explicit).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --all) ALL=1; shift ;;
        --system) SYS=1; shift ;;
        --purge-cache) PURGE_CACHE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown argument: $1 (see --help)" ;;
        *) [[ -z "$OPT_NAME" ]] && OPT_NAME="$1" || die "only one instance name allowed"
           shift ;;
    esac
done

sudo_run() {
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[sudo] $*"
    else
        sudo "$@"
    fi
}

rmrf() {
    local p="$1"
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[dry-run] rm -rf $p"
    else
        rm -rf "$p"
        say "removed: $p"
    fi
}

# ── discover instances ───────────────────────────────────────────────────────
step "scanning $CT_DIR for proton-ds instances"
[[ -d "$CT_DIR" ]] || die "$CT_DIR does not exist — nothing to uninstall"

declare -a DS_NAMES=() DS_DIRS=()
while IFS= read -r d; do
    DS_NAMES+=("$(basename "$d")")
    DS_DIRS+=("$d")
done < <(find "$CT_DIR" -mindepth 1 -maxdepth 1 -type d -name "*$DS_SUFFIX" -exec test -f "{}/$MANIFEST_NAME" \; -print 2>/dev/null | sort)

if [[ ${#DS_NAMES[@]} -eq 0 ]]; then
    say "no proton-ds instances found (nothing with $MANIFEST_NAME)"
else
    say "instances found: ${#DS_NAMES[@]}"
    for i in "${!DS_NAMES[@]}"; do
        local_v="$(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m.get("verified"), m.get("source_instance","?"))' "${DS_DIRS[$i]}/$MANIFEST_NAME" 2>/dev/null || echo "manifest-unreadable")"
        printf '  [%d] %-28s verified=%s src=%s\n' "$((i+1))" "${DS_NAMES[$i]}" "$(echo "$local_v" | cut -d' ' -f1)" "$(echo "$local_v" | cut -d' ' -f2)"
    done
fi

# ── selection ────────────────────────────────────────────────────────────────
declare -a TO_REMOVE=()
if [[ $ALL -eq 1 ]]; then
    TO_REMOVE=("${DS_DIRS[@]}")
elif [[ -n "$OPT_NAME" ]]; then
    found=""
    for i in "${!DS_DIRS[@]}"; do
        [[ "${DS_NAMES[$i]}" == "$OPT_NAME" ]] && found="${DS_DIRS[$i]}"
    done
    [[ -n "$found" ]] || die "instance '$OPT_NAME' not found (or has no $MANIFEST_NAME)
  what to do: pick from the list above, or run with --all"
    TO_REMOVE=("$found")
else
    if [[ ${#DS_NAMES[@]} -eq 0 ]]; then
        :
    elif [[ -t 0 ]]; then
        printf 'Select the instance to remove (1-%d, or A for all): ' "${#DS_NAMES[@]}"
        read -r answer
        if [[ "${answer,,}" == "a" ]]; then
            TO_REMOVE=("${DS_DIRS[@]}")
        elif [[ "$answer" =~ ^[0-9]+$ ]] && (( answer >= 1 && answer <= ${#DS_NAMES[@]} )); then
            TO_REMOVE=("${DS_DIRS[$((answer-1))]}")
        else
            die "expected a number 1-${#DS_NAMES[@]} or A, got '$answer'"
        fi
    else
        die "non-interactive run: pass an instance NAME, --all, or run with a terminal"
    fi
fi

# ── remove instances ─────────────────────────────────────────────────────────
if [[ ${#TO_REMOVE[@]} -gt 0 ]]; then
    step "removing ${#TO_REMOVE[@]} instance(s)"
    for d in "${TO_REMOVE[@]}"; do
        warn "note: uninstalling does NOT touch a running session — stop the daemon before launching games on the stock instance"
        rmrf "$d"
    done
else
    say "no instances selected for removal"
fi

# ── system components ────────────────────────────────────────────────────────
DAEMON_DST="/usr/bin/ds4linux-daemon"
DS4CTL_DST="/usr/local/bin/ds4ctl"

if [[ $SYS -eq 1 ]]; then
    step "system components (sudo)"
    if [[ $DRY_RUN -eq 0 ]] && pgrep -x ds4linux-daemon >/dev/null 2>&1; then
        say "daemon is running — stopping it first (sudo ds4ctl stop)"
        sudo_run ds4ctl stop || warn "ds4ctl stop failed — continuing (check 'sudo ds4ctl status')"
    fi
    if [[ -e "$DAEMON_DST.pdsbak" ]]; then
        say "daemon: restoring the pre-proton-ds backup"
        sudo_run mv "$DAEMON_DST.pdsbak" "$DAEMON_DST"
    else
        say "daemon: no .pdsbak backup — removing the binary"
        sudo_run rm -f "$DAEMON_DST"
    fi
    if [[ -e "$DS4CTL_DST.pdsbak" ]]; then
        say "ds4ctl: restoring the pre-proton-ds backup"
        sudo_run mv "$DS4CTL_DST.pdsbak" "$DS4CTL_DST"
    else
        say "ds4ctl: no .pdsbak backup — removing the binary"
        sudo_run rm -f "$DS4CTL_DST"
    fi
    say "system components done (daemon: $DAEMON_DST, ds4ctl: $DS4CTL_DST)"
else
    say "system components (daemon, ds4ctl) left in place — pass --system to remove them"
fi

# ── cache ────────────────────────────────────────────────────────────────────
if [[ $PURGE_CACHE -eq 1 ]]; then
    step "purging cache: $CACHE_DIR"
    rmrf "$CACHE_DIR"
else
    if [[ -d "$CACHE_DIR" ]]; then
        say "cache kept: $CACHE_DIR ($(du -sm "$CACHE_DIR" 2>/dev/null | cut -f1) MB — pass --purge-cache to delete, or keep it for faster reinstall)"
    fi
fi

if [[ $DRY_RUN -eq 1 ]]; then
    say "DRY-RUN complete: action plan printed, no changes made"
else
    say "uninstall complete"
fi
