#!/usr/bin/env bash
# proton-ds setup — install the DS-emulation stack into a -DS copy of a Proton instance.
# Model: scan compatibilitytools.d -> pick an instance -> cp -a <name> <name>-DS
# -> patch ONLY the copy. Stock is never touched. Prefixes are never touched
# (wineboot propagates driver copies from the dist of the instance a game runs on;
# proven by hash audit 2026-08-17).
set -euo pipefail

VERSION="0.1.0-dev"
CT_DIR="${CT_DIR:-$HOME/.local/share/Steam/compatibilitytools.d}"
CACHE_DIR="${PROTON_DS_CACHE:-$HOME/.cache/proton-ds}"
DS_SUFFIX="-DS"
MANIFEST_NAME="proton-ds.json"
HEADROOM_MB=300
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_TAG="v0.1-winebus-prebuilts"
RELEASE_BASE="${PROTON_DS_RELEASE_BASE:-https://github.com/NaGGaN-Z/proton-ds/releases/download/$RELEASE_TAG}"
WINE_COMMIT="9578fa3613f3379179b576968bc77c8161ab6ea8"
WINEBUS_REL_X64="files/lib/wine/x86_64-unix/winebus.so"
WINEBUS_SIZE_SOFT_MIN=90000
WINEBUS_SIZE_SOFT_MAX=120000
BUILD_LOG_NAME="build.log"

DRY_RUN=0
FORCE=0
OPT_INSTANCE=""
OPT_SKIP_VERIFY=0
OPT_WITH_BUILD=0
DS_DIR=""
CREATED_DS=0

say() { printf '\033[1;36m[proton-ds]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[proton-ds WARN]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[proton-ds FATAL]\033[0m %s\n' "$*" >&2; exit 1; }
step() { say "==> $*"; }

usage() {
    cat <<EOF
proton-ds setup $VERSION

Usage: setup.sh [--instance NAME] [--force] [--dry-run] [--skip-verify] [--with-build]

  --instance NAME   pick instance NAME, skipping the interactive list
  --force           replace an existing NAME-DS (default: refuse)
  --dry-run         print the full action plan, make zero changes
  --skip-verify     skip the verify gate (manifest: verified=false)
  --with-build      prefer the source-build path for winebus (non-interactive
                    answer 'y' to the build question; no prebuilt download)

Environment: CT_DIR (compatibilitytools.d), PROTON_DS_CACHE (~/.cache/proton-ds).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --instance) OPT_INSTANCE="${2:-}"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --skip-verify) OPT_SKIP_VERIFY=1; shift ;;
        --with-build) OPT_WITH_BUILD=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done

cleanup() {
    local rc=$?
    if [[ -n "$DS_DIR" && -d "$DS_DIR" && $CREATED_DS -eq 1 ]]; then
        if [[ $DRY_RUN -eq 1 ]]; then
            say "abort: [dry-run] would remove the incomplete instance $DS_DIR"
        else
            warn "abort: removing incomplete instance $DS_DIR (the stock instance is untouched)"
            rm -rf "$DS_DIR"
        fi
    fi
    exit "$rc"
}
trap cleanup EXIT

# ── deps ─────────────────────────────────────────────────────────────────────
step "deps: python3, coreutils, curl"
command -v python3 >/dev/null 2>&1 || die "python3 not found (required for hex-patching and the manifest)"
command -v curl >/dev/null 2>&1 || die "curl not found (required for the winebus prebuilt path)"
for c in cp df du sha256sum mktemp; do
    command -v "$c" >/dev/null 2>&1 || die "$c not found (coreutils)"
done
say "deps: ok"

# ── Phase 0: scan instances ──────────────────────────────────────────────────
step "scanning $CT_DIR"
[[ -d "$CT_DIR" ]] || die "$CT_DIR does not exist
  likely cause: Steam with compatibility tools was never installed on this machine
  what to do: install Steam and a Proton build first, then re-run setup"

declare -a INST_NAMES=() INST_VERSIONS=() INST_STATES=() INST_DIRS=()
while IFS= read -r d; do
    name="$(basename "$d")"
    ver="?"
    if [[ -f "$d/version" ]]; then
        ver="$(head -n1 "$d/version" 2>/dev/null || echo '?')"
    fi
    state="stock"
    if [[ -f "$d/$MANIFEST_NAME" ]]; then
        state="proton-ds"
    elif [[ "$name" == *"$DS_SUFFIX" ]]; then
        state="ds-copy"
    elif grep -qa "Sony Computer En" "$d/files/lib/wine/x86_64-unix/winebus.so" 2>/dev/null; then
        state="winebus-patched"
    fi
    INST_NAMES+=("$name"); INST_VERSIONS+=("$ver"); INST_STATES+=("$state"); INST_DIRS+=("$d")
done < <(find "$CT_DIR" -mindepth 1 -maxdepth 1 -type d -exec test -d '{}/files/lib/wine' \; -print 2>/dev/null | sort)

[[ ${#INST_NAMES[@]} -gt 0 ]] || die "no Proton instance with files/lib/wine found in $CT_DIR
  what to do: install a Proton build (Steam will place it there), then re-run setup"

say "instances found: ${#INST_NAMES[@]}"
for i in "${!INST_NAMES[@]}"; do
    printf '  [%d] %-28s %-16s %s\n' "$((i+1))" "${INST_NAMES[$i]}" "${INST_VERSIONS[$i]}" "${INST_STATES[$i]}"
done

# ── instance selection ───────────────────────────────────────────────────────
choice_idx=""
if [[ -n "$OPT_INSTANCE" ]]; then
    for i in "${!INST_NAMES[@]}"; do
        [[ "${INST_NAMES[$i]}" == "$OPT_INSTANCE" ]] && choice_idx="$i"
    done
    [[ -n "$choice_idx" ]] || die "instance '$OPT_INSTANCE' not found in the list above
  what to do: pick one of the listed names (or run without --instance for the interactive list)"
else
    [[ -t 0 ]] || die "non-interactive run: no terminal for the instance list
  what to do: pass --instance NAME (see the list printed above, or run with a terminal)"
    printf 'Select the instance to copy (1-%d): ' "${#INST_NAMES[@]}"
    read -r answer
    [[ "$answer" =~ ^[0-9]+$ ]] || die "expected a number between 1 and ${#INST_NAMES[@]}, got '$answer'"
    [[ "$answer" -ge 1 && "$answer" -le "${#INST_NAMES[@]}" ]] || die "number out of range: $answer (expected 1-${#INST_NAMES[@]})"
    choice_idx="$((answer-1))"
fi

SRC_NAME="${INST_NAMES[$choice_idx]}"
SRC_DIR="${INST_DIRS[$choice_idx]}"
SRC_VER="${INST_VERSIONS[$choice_idx]}"
SRC_STATE="${INST_STATES[$choice_idx]}"
step "selected: $SRC_NAME ($SRC_VER, $SRC_STATE)"

[[ "$SRC_STATE" == "proton-ds" ]] && die "selected instance is already a proton-ds build; the source must be a stock instance
  what to do: pick a stock instance from the list"
[[ "$SRC_STATE" == "ds-copy" ]] && warn "the source $SRC_NAME looks like a -DS copy itself; patching a clean stock instance is recommended"
[[ "$SRC_STATE" == "winebus-patched" ]] && warn "winebus in $SRC_NAME already contains a proton-ds marker — the copy will inherit a patched winebus"

DS_DIR="$CT_DIR/$SRC_NAME$DS_SUFFIX"
if [[ -e "$DS_DIR" ]]; then
    if [[ $FORCE -eq 1 ]]; then
        step "--force: removing existing $DS_DIR"
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] rm -rf $DS_DIR"
        else
            rm -rf "$DS_DIR"
        fi
    else
        die "$DS_DIR already exists
  what to do: run uninstall.sh to remove it, or pass --force to replace it"
    fi
fi

# ── disk space check ─────────────────────────────────────────────────────────
step "checking free disk space"
src_size_mb=$(( $(du -sm "$SRC_DIR" | cut -f1) ))
avail_mb=$(( $(df -P -m "$CT_DIR" | awk 'NR==2{print $4}') ))
need_mb=$(( src_size_mb + HEADROOM_MB ))
say "instance size: ${src_size_mb} MB, free on partition: ${avail_mb} MB, needed: ~${need_mb} MB (+${HEADROOM_MB} MB headroom)"
if (( avail_mb < need_mb )); then
    die "not enough disk space: ${avail_mb} MB free, ~${need_mb} MB needed
  (copy ${src_size_mb} MB + ${HEADROOM_MB} MB headroom)
  what to do: free up space in $CT_DIR and retry"
fi

# ── copy ─────────────────────────────────────────────────────────────────────
step "copying $SRC_NAME -> ${SRC_NAME}${DS_SUFFIX} (cp -a)"
if [[ $DRY_RUN -eq 1 ]]; then
    say "[dry-run] cp -a $SRC_DIR $DS_DIR"
else
    cp -a "$SRC_DIR" "$DS_DIR"
    CREATED_DS=1
    say "copy ready: $DS_DIR"
fi

# ── manifest ─────────────────────────────────────────────────────────────────
step "manifest $MANIFEST_NAME"
manifest_write() {
    local content="$1"
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[dry-run] would write the manifest:"
        printf '%s\n' "$content"
        return
    fi
    printf '%s\n' "$content" > "$DS_DIR/$MANIFEST_NAME"
    say "manifest written: $DS_DIR/$MANIFEST_NAME"
}

now="$(date -Iseconds)"
manifest_content="$(python3 - "$VERSION" "$now" "$SRC_NAME" "$SRC_VER" <<'PYEOF'
import json, sys
m = {
    "proton_ds_version": sys.argv[1],
    "created": sys.argv[2],
    "source_instance": sys.argv[3],
    "source_version": sys.argv[4],
    "steps": [],
    "winebus": {"method": None, "sha256": None},
    "drivers": {},
    "system": {"daemon": None, "ds4ctl": None},
    "verified": False,
    "verify_reason": None,
}
print(json.dumps(m, indent=2, ensure_ascii=False))
PYEOF
)" || die "failed to generate the manifest (python3 error above, if any)
  safe abort: the incomplete -DS copy will be removed"
manifest_write "$manifest_content"
if [[ $DRY_RUN -eq 0 ]]; then
    python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$DS_DIR/$MANIFEST_NAME" \
        || die "manifest file is not valid JSON — this is a proton-ds bug, please report it"
    say "manifest is valid (JSON ok)"
fi

manifest_add_step() {
    local entry="$1"
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[dry-run] manifest: += $entry"
        return
    fi
    python3 - "$DS_DIR/$MANIFEST_NAME" "$entry" <<'PYEOF'
import json, sys
path, entry = sys.argv[1], json.loads(sys.argv[2])
m = json.load(open(path))
m["steps"].append(entry)
json.dump(m, open(path, "w"), indent=2, ensure_ascii=False)
PYEOF
    say "manifest: += $entry"
}

manifest_set() {
    local key="$1" val="$2"
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[dry-run] manifest: $key = $val"
        return
    fi
    python3 - "$DS_DIR/$MANIFEST_NAME" "$key" "$val" <<'PYEOF'
import json, sys
path, key = sys.argv[1], sys.argv[2]
val = json.loads(sys.argv[3])
m = json.load(open(path))
cur = m
for part in key.split(".")[:-1]:
    cur = cur.setdefault(part, {})
cur[key.split(".")[-1]] = val
json.dump(m, open(path, "w"), indent=2, ensure_ascii=False)
PYEOF
    say "manifest: $key = $val"
}

# ── Phase 2: winebus (hybrid: prebuilt -> build) ─────────────────────────────
deploy_winebus() {
    step "winebus: prebuilt path ($RELEASE_BASE)"
    local target="$DS_DIR/$WINEBUS_REL_X64"
    local log="$CACHE_DIR/$BUILD_LOG_NAME"
    if [[ $DRY_RUN -eq 0 ]]; then
        [[ -f "$target" ]] || die "the copy has no $WINEBUS_REL_X64
  the dist structure is unexpected for this instance
  what to do: report this (instance name + Proton version); nothing was modified"
    fi

    local lookup
    if [[ $DRY_RUN -eq 1 ]]; then
        # dry-run не ходит в сеть: показываем обе ветки без реального manifest
        lookup='{"found": false, "available": ["<would be fetched from the release>"]}'
    else
        mkdir -p "$CACHE_DIR"
        local manifest_ok=1
        if ! curl -fsSL --max-time 30 -o "$CACHE_DIR/manifest.json" "$RELEASE_BASE/manifest.json"; then
            if curl -fsI --max-time 10 "https://github.com" >/dev/null 2>&1; then
                warn "manifest.json unavailable from $RELEASE_BASE, but the network is reachable
  (the release $RELEASE_TAG is missing or broken — a proton-ds problem worth reporting)
  falling through to the source-build offer"
            else
                die "failed to download manifest.json from $RELEASE_BASE
  likely cause: no network connection. Both the prebuilt and the source-build path require network access (the build clones the Wine tree)
  what to do: connect to the network and re-run setup
  safe abort: the incomplete -DS copy will be removed; the stock instance is untouched"
            fi
            manifest_ok=0
        else
            say "manifest.json downloaded: $CACHE_DIR/manifest.json"
        fi
        if [[ $manifest_ok -eq 1 ]]; then
            lookup="$(python3 - "$CACHE_DIR/manifest.json" "$SRC_NAME" <<'PYEOF'
import json, sys
m = json.load(open(sys.argv[1]))
entry = m.get(sys.argv[2])
if entry is None:
    print(json.dumps({"found": False, "available": sorted(m.keys())}))
else:
    print(json.dumps({"found": True, **entry}))
PYEOF
)" || die "failed to parse manifest.json (invalid JSON?)
  this is likely a proton-ds release problem — please report it
  safe abort: the incomplete -DS copy will be removed"
        else
            lookup='{"found": false, "available": ["<manifest unavailable>"]}'
        fi
    fi

    local file sha size
    file="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("file",""))' "$lookup")"
    sha="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("sha256",""))' "$lookup")"
    size="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("size",0))' "$lookup")"
    local found
    found="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["found"])' "$lookup")"

    if [[ "$found" != "True" ]]; then
        local avail
        avail="$(python3 -c 'import json,sys; print(", ".join(json.loads(sys.argv[1])["available"]))' "$lookup")"
        warn "no prebuilt winebus for instance '$SRC_NAME' in the Releases"
        say "available prebuilts: ${avail:-<none>}"
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] would ask: build winebus from source? [y/N]"
            say "[dry-run] on N: abort, trap removes $DS_DIR"
            return
        fi
        local answer="n"
        if [[ $OPT_WITH_BUILD -eq 1 ]]; then
            answer="y"
        elif [[ -t 0 ]]; then
            printf 'Build winebus from source (~10-30 min, build toolchain required)? [y/N] '
            read -r answer
        else
            warn "non-interactive run — assuming N (pass --with-build to force the source build)"
        fi
        if [[ "${answer,,}" == "y" ]]; then
            build_winebus
            return
        fi
        if [[ $DRY_RUN -eq 1 ]]; then
            return
        fi
        die "no prebuilt available and the source build was declined
  what to do: use one of the listed prebuilt instances, or re-run and answer 'y' to build
  safe abort: the -DS copy will be removed; the stock instance is untouched"
    fi

    say "prebuilt found: $file (sha256 ${sha:0:12}..., ${size} B)"
    local cached="$CACHE_DIR/$file"
    if [[ -f "$cached" ]] && [[ "$(sha256sum "$cached" | cut -d' ' -f1)" == "$sha" ]]; then
        say "cache is valid, skipping download: $cached"
    else
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] curl -fL $RELEASE_BASE/$file -> $cached (+sha256 check)"
        else
            if ! curl -fL --max-time 300 -o "$cached" "$RELEASE_BASE/$file"; then
                die "failed to download $file (network interrupted?)
  what to do: check the connection and re-run setup
  safe abort: the incomplete -DS copy will be removed"
            fi
        fi
    fi
    if [[ $DRY_RUN -eq 0 ]]; then
        local got
        got="$(sha256sum "$cached" | cut -d' ' -f1)"
        [[ "$got" == "$sha" ]] || die "sha256 mismatch for $file
  downloaded: $got
  expected:   $sha
  likely cause: corrupted download or poisoned cache
  what to do: delete $cached and re-run setup"
        [[ "$(stat -c%s "$cached")" == "$size" ]] || die "artifact size mismatch for $file: $(stat -c%s "$cached") B, manifest says $size B
  what to do: delete $cached and re-run setup"
        step "installing winebus: $target"
        cp "$cached" "$target"
        chmod 755 "$target"
        local installed_sha
        installed_sha="$(sha256sum "$target" | cut -d' ' -f1)"
        say "installed: $target (sha256 $installed_sha)"
        say "i386-unix/winebus.so left untouched by design (the unix side loads from x86_64)"
        manifest_set "winebus.method" '"prebuilt"'
        manifest_set "winebus.sha256" "\"$installed_sha\""
        manifest_add_step "\"winebus: prebuilt $file ($installed_sha)\""
    else
        say "[dry-run] cp $cached -> $target (chmod 755)"
    fi
}

build_winebus() {
    step "winebus: source-build path (wine@$WINE_COMMIT + ge patches + v13)"
    local src="$CACHE_DIR/wine-src" bld="$CACHE_DIR/wine-build64"

    # deps
    step "build deps check"
    local missing=""
    for c in gcc autoreconf autoconf make bison flex perl git strip; do
        command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
    done
    if [[ -n "$missing" ]]; then
        die "build toolchain incomplete, missing:$missing
  install the packages that provide them, e.g. (Arch): sudo pacman -S $missing
  (Debian/Ubuntu): sudo apt install$(echo "$missing" | sed 's/autoreconf/autoconf automake/; s/gcc/gcc build-essential/')
  then re-run setup
  safe abort: the incomplete -DS copy will be removed; the stock instance is untouched"
    fi
    say "build deps: ok"

    # clone
    step "cloning ValveSoftware/wine@$WINE_COMMIT (shallow) -> $src"
    if [[ -d "$src/.git" ]]; then
        say "source tree already present: $src (reusing; run uninstall.sh --purge-cache to reset)"
    else
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] git clone --filter=blob:none https://github.com/ValveSoftware/wine $src && git -C $src checkout $WINE_COMMIT"
        else
            mkdir -p "$CACHE_DIR"
            if ! git clone --filter=blob:none https://github.com/ValveSoftware/wine "$src" >>"$log" 2>&1 \
               || ! git -C "$src" checkout "$WINE_COMMIT" >>"$log" 2>&1; then
                die "failed to clone/checkout wine@$WINE_COMMIT (details: $log)
  likely cause: network problem
  what to do: check the connection and re-run setup
  safe abort: the -DS copy will be removed"
            fi
            say "clone done: $(git -C "$src" log --oneline -1)"
        fi
    fi

    # patches
    step "applying patches: ge/0018, ge/0036, v13"
    local patches=(
        "$SCRIPT_DIR/../patches/winebus/ge/0018-winebus-remove-hidraw-device-on-fatal-read-error.patch"
        "$SCRIPT_DIR/../patches/winebus/ge/0036-winebus-ignore-duplicate-udev-devnodes.patch"
        "$SCRIPT_DIR/../patches/winebus/v13-sony-gamepad.patch"
    )
    for p in "${patches[@]}"; do
        [[ -f "$p" ]] || die "patch file missing from the proton-ds repo: $p
  what to do: this is a broken installation of proton-ds — clone the repo fully"
        if git -C "$src" apply --check "$p" 2>>"$log"; then
            if [[ $DRY_RUN -eq 1 ]]; then
                say "[dry-run] git -C $src apply $(basename "$p")"
            else
                git -C "$src" apply "$p" >>"$log" 2>&1 && say "applied: $(basename "$p")"
            fi
        elif git -C "$src" apply --check --reverse "$p" 2>>"$log"; then
            say "already applied: $(basename "$p") (skipping)"
        else
            die "patch $(basename "$p") does not apply to wine@$WINE_COMMIT
  likely cause: the source tree in $src was modified by something else
  what to do: run uninstall.sh --purge-cache and re-run setup
  safe abort: the -DS copy will be removed"
        fi
    done

    # pregeneration: maintainer generators must run before configure
    # (configure needs their outputs; empirically proven on the reference
    # build: byte-exact reproduction modulo build-id)
    step "pregeneration: make_vulkan, make_specfiles, make_requests"
    local pregen_log="$CACHE_DIR/pregen.log"
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[dry-run] (cd $src/dlls/winevulkan && ./make_vulkan -x vk.xml -X video.xml)"
        say "[dry-run] $src/tools/make_specfiles && $src/tools/make_requests"
    else
        if ! (cd "$src/dlls/winevulkan" && ./make_vulkan -x vk.xml -X video.xml) >>"$pregen_log" 2>&1; then
            die "make_vulkan failed (details: $pregen_log)
  what to do: report this (generator vs vk.xml mismatch)
  safe abort: the -DS copy will be removed"
        fi
        say "make_vulkan: ok (vulkan.h, thunks)"
        # make_specfiles/make_requests resolve .spec paths relative to the
        # source root — must run with cwd = $src
        if ! (cd "$src" && ./tools/make_specfiles) >>"$pregen_log" 2>&1; then
            die "make_specfiles failed (details: $pregen_log)
  safe abort: the -DS copy will be removed"
        fi
        say "make_specfiles: ok (ntsyscalls.h, win32syscalls.h)"
        if ! (cd "$src" && ./tools/make_requests) >>"$pregen_log" 2>&1; then
            die "make_requests failed (details: $pregen_log)
  safe abort: the -DS copy will be removed"
        fi
        say "make_requests: ok (request_handlers.h)"
    fi

    # preflight: byte-exact control against the deployed tree
    if [[ $DRY_RUN -eq 0 ]]; then
        local h
        h="$(sha256sum "$src/dlls/winebus.sys/bus_udev.c" | cut -d' ' -f1)"
        if [[ "$h" != "6df70822cff61dfb3747c6c198ceb5d9a7801d8402ba25a5dd5e9e7a00ec7382" ]]; then
            warn "bus_udev.c hash $h != deployed-tree golden — continuing anyway (golden is GE-Proton11-3-specific)"
        else
            say "preflight: bus_udev.c matches the deployed-tree golden — recipe verified"
        fi
    fi

    # configure
    step "autoreconf + configure (out-of-tree: $bld)"
    if [[ ! -f "$src/configure" ]]; then
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] (cd $src && autoreconf -f)"
        else
            if ! (cd "$src" && autoreconf -f) >>"$log" 2>&1; then
                die "autoreconf failed in $src (details: $log)
  likely cause: missing autoconf/automake pieces
  safe abort: the -DS copy will be removed"
            fi
        fi
    fi
    # absolute srcdir: the reference build used an absolute source path
    # (affects __FILE__ strings in TRACE — cosmetic, but keeps builds consistent)
    local abssrc
    abssrc="$(cd "$src" && pwd)"
    if [[ ! -f "$bld/Makefile" ]]; then
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] mkdir -p $bld && (cd $bld && $abssrc/configure --enable-win64 --without-mingw --disable-tests)"
        else
            mkdir -p "$bld"
            if ! (cd "$bld" && "$abssrc/configure" --enable-win64 --without-mingw --disable-tests) >>"$log" 2>&1; then
                die "configure failed (details: $log)
  likely cause: missing development headers (e.g. libudev, ALSA, fontconfig)
  what to do: install the missing -dev packages and re-run setup
  safe abort: the -DS copy will be removed"
            fi
        fi
    else
        say "build dir already configured: $bld (reusing)"
    fi

    # module build
    step "building winebus.sys module (first build takes ~10-30 min)"
    say "full log: $log (tail follows below)"
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[dry-run] make -C $bld/dlls/winebus.sys"
        return
    fi
    if ! make -C "$bld/dlls/winebus.sys" >>"$log" 2>&1; then
        tail -30 "$log" || true
        die "make winebus.sys failed (last 30 lines above, full log: $log)
  what to do: fix the toolchain/headers issue and re-run setup
  safe abort: the -DS copy will be removed"
    fi
    tail -5 "$log" || true

    # artifact
    local built="$bld/dlls/winebus.sys/winebus.so"
    [[ -f "$built" ]] || die "build finished but $built not found — this is a proton-ds bug, please report it
  safe abort: the -DS copy will be removed"

    step "stripping and installing winebus"
    strip --strip-unneeded -o "$bld/winebus.stripped.so" "$built" || die "strip failed on $built"
    local size target
    size="$(stat -c%s "$bld/winebus.stripped.so")"
    target="$DS_DIR/$WINEBUS_REL_X64"
    if (( size < WINEBUS_SIZE_SOFT_MIN || size > WINEBUS_SIZE_SOFT_MAX )); then
        warn "winebus.so size ${size} B is outside the expected ${WINEBUS_SIZE_SOFT_MIN}-${WINEBUS_SIZE_SOFT_MAX} KB window
  (the deployed-proven build is 97032 B; a different toolchain legitimately produces a different size)
  continuing — the verify gate (later in setup) is the real correctness check"
    else
        say "size check (soft): ${size} B within ${WINEBUS_SIZE_SOFT_MIN}-${WINEBUS_SIZE_SOFT_MAX} — ok"
    fi
    cp "$bld/winebus.stripped.so" "$target"
    chmod 755 "$target"
    local installed_sha
    installed_sha="$(sha256sum "$target" | cut -d' ' -f1)"
    say "installed: $target (sha256 $installed_sha)"
    say "i386-unix/winebus.so left untouched by design (the unix side loads from x86_64)"
    manifest_set "winebus.method" '"build"'
    manifest_set "winebus.sha256" "\"$installed_sha\""
    manifest_add_step "\"winebus: built from wine@$WINE_COMMIT (${size} B, $installed_sha)\""
}

deploy_winebus

# ── Phase 3: driver hex-patches ──────────────────────────────────────────────
patch_drivers() {
    step "driver hex-patches: winexinput (swap) x2, hidclass (guid) x2"
    local hexpy="$SCRIPT_DIR/hexpatch.py"
    [[ -f "$hexpy" ]] || die "hexpatch.py not found at $hexpy
  what to do: this is a broken proton-ds installation — clone the repo fully"

    local targets=(
        "x86_64-windows/winexinput.sys:swap"
        "i386-windows/winexinput.sys:swap"
        "x86_64-windows/hidclass.sys:guid"
        "i386-windows/hidclass.sys:guid"
    )
    local rel kind f rc patched=0 skipped=0
    for entry in "${targets[@]}"; do
        rel="${entry%%:*}"; kind="${entry##*:}"
        f="$DS_DIR/files/lib/wine/$rel"
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] hexpatch.py check + $kind $f"
            continue
        fi
        [[ -f "$f" ]] || die "driver file missing in the -DS copy: $f
  the dist structure is unexpected for this instance
  what to do: report this (instance name + Proton version)"
        # idempotency: check state first
        set +e
        python3 "$hexpy" check "$f" --kind "$kind" >/dev/null 2>&1
        rc=$?
        set -e
        if [[ $rc -eq 10 ]]; then
            warn "already patched: $rel (skipping)"
            skipped=$((skipped+1))
            manifest_set "drivers.$(basename "$f").$kind" '"patched-before"'
            continue
        elif [[ $rc -eq 11 ]]; then
            die "driver state unclassifiable: $rel
  the hexpatch output above explains what it found
  likely cause: this driver binary is not compatible with proton-ds (different GE/Wine version)
  what to do: try a different Proton instance, or report the case (instance name + Proton version)
  safe abort: the -DS copy will be removed; the stock instance is untouched"
        fi
        say "patching ($kind): $rel"
        if ! python3 "$hexpy" "$kind" "$f"; then
            die "hex-patch failed for $rel
  the hexpatch error above includes the likely cause and details
  safe abort: the -DS copy will be removed; the stock instance is untouched"
        fi
        # post-check
        set +e
        python3 "$hexpy" check "$f" --kind "$kind" >/dev/null 2>&1
        rc=$?
        set -e
        [[ $rc -eq 10 ]] || die "post-patch verification failed for $rel (check exit $rc, expected patched)
  this is a proton-ds bug — please report it
  safe abort: the -DS copy will be removed"
        local h
        h="$(sha256sum "$f" | cut -d' ' -f1)"
        say "patched: $rel (sha256 $h)"
        manifest_set "drivers.$(basename "$f").$kind" "\"$h\""
        patched=$((patched+1))
    done
    if [[ $DRY_RUN -eq 0 ]]; then
        say "hex-patches: $patched applied, $skipped skipped (already patched)"
        manifest_add_step "\"drivers: hex-patched (winexinput swap x2, hidclass guid x2; $patched applied, $skipped skipped)\""
    fi
}

patch_drivers

# ── Phase 4: daemon + ds4ctl (sudo block) ────────────────────────────────────
DS4LINUX_REPO="https://github.com/NaGGaN-Z/ds4linux"
DS4LINUX_BRANCH="proton-compat"
DAEMON_DST="/usr/bin/ds4linux-daemon"
DS4CTL_DST="/usr/local/bin/ds4ctl"

sudo_run() {
    if [[ $DRY_RUN -eq 1 ]]; then
        say "[sudo] $*"
    else
        sudo "$@"
    fi
}

install_system() {
    step "daemon + ds4ctl (system install, sudo)"
    local repo="$CACHE_DIR/ds4linux"
    local log="$CACHE_DIR/$BUILD_LOG_NAME"

    # deps
    local missing=""
    for c in cmake ninja g++; do
        command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
    done
    if [[ -n "$missing" ]]; then
        die "daemon build deps missing:$missing
  install the packages that provide them (Arch: sudo pacman -S$missing;
  Debian/Ubuntu: sudo apt install$(echo "$missing" | sed 's/g++/g++ cmake ninja-build/'))
  then re-run setup
  note: the daemon is REQUIRED for DS4 emulation (the virtual controller);
  without it setup cannot finish"
    fi
    say "daemon build deps: ok"

    # clone
    step "cloning ds4linux (fork, branch $DS4LINUX_BRANCH)"
    if [[ -d "$repo/.git" ]]; then
        say "repo already present: $repo (reusing)"
    else
        if [[ $DRY_RUN -eq 1 ]]; then
            say "[dry-run] git clone -b $DS4LINUX_BRANCH $DS4LINUX_REPO $repo"
        else
            mkdir -p "$CACHE_DIR"
            if ! git clone -b "$DS4LINUX_BRANCH" "$DS4LINUX_REPO" "$repo" >>"$log" 2>&1; then
                die "failed to clone $DS4LINUX_REPO (branch $DS4LINUX_BRANCH)
  likely cause: network problem
  what to do: check the connection and re-run setup"
            fi
            say "clone done: $(git -C "$repo" log --oneline -1)"
        fi
    fi

    # build
    step "building daemon (cmake + ninja)"
    local daemon_bin="$repo/build/daemon/ds4linux-daemon"
    if [[ -f "$daemon_bin" ]]; then
        say "daemon binary already built: $daemon_bin (reusing)"
    elif [[ $DRY_RUN -eq 1 ]]; then
        say "[dry-run] cmake -S $repo -B $repo/build -G Ninja && ninja -C $repo/build"
    else
        if ! cmake -S "$repo" -B "$repo/build" -G Ninja >>"$log" 2>&1 \
           || ! ninja -C "$repo/build" >>"$log" 2>&1; then
            die "daemon build failed (details: $log)
  what to do: fix the toolchain issue and re-run setup"
        fi
        [[ -f "$daemon_bin" ]] || die "build finished but $daemon_bin not found — this is a proton-ds bug, please report it"
        say "daemon built: $daemon_bin"
    fi

    # install with one-time backups
    step "installing daemon and ds4ctl (sudo)"
    if [[ -e "$DAEMON_DST" && ! -e "$DAEMON_DST.pdsbak" ]]; then
        sudo_run cp "$DAEMON_DST" "$DAEMON_DST.pdsbak"
        say "one-time backup of the existing daemon: $DAEMON_DST.pdsbak"
    fi
    sudo_run install -m755 "$daemon_bin" "$DAEMON_DST"
    say "daemon installed: $DAEMON_DST"

    local ds4ctl_src="$SCRIPT_DIR/ds4ctl"
    [[ -f "$ds4ctl_src" ]] || die "ds4ctl not found at $ds4ctl_src
  what to do: this is a broken proton-ds installation — clone the repo fully"
    if [[ -e "$DS4CTL_DST" && ! -e "$DS4CTL_DST.pdsbak" ]]; then
        sudo_run cp "$DS4CTL_DST" "$DS4CTL_DST.pdsbak"
        say "one-time backup of the existing ds4ctl: $DS4CTL_DST.pdsbak"
    fi
    sudo_run install -m755 "$ds4ctl_src" "$DS4CTL_DST"
    say "ds4ctl installed: $DS4CTL_DST"

    if [[ $DRY_RUN -eq 0 ]]; then
        say "daemon is installed but NOT started (lifecycle is yours: sudo ds4ctl start|stop|status)"
        manifest_set "system.daemon" "\"$DAEMON_DST\""
        manifest_set "system.ds4ctl" "\"$DS4CTL_DST\""
        manifest_add_step "\"system: daemon + ds4ctl installed (sudo; one-time .pdsbak backups)\""
    fi
}

install_system

# ── Phase 5: verify gate ─────────────────────────────────────────────────────
verify_gate() {
    step "verify gate"
    local verifiers_dir="$SCRIPT_DIR/../verifiers"
    [[ -d "$verifiers_dir" ]] || die "verifiers/ not found at $verifiers_dir
  what to do: this is a broken proton-ds installation — clone the repo fully"

    if [[ $OPT_SKIP_VERIFY -eq 1 ]]; then
        warn "verify gate skipped (--skip-verify)"
        manifest_set "verified" "false"
        manifest_set "verify_reason" '"skipped (--skip-verify)"'
        warn "the -DS instance is installed but NOT verified — run games at your own risk"
        return
    fi

    # winegcc dep
    command -v winegcc >/dev/null 2>&1 || die "winegcc not found — the verify gate needs it to build the gameless verifiers
  what to do: install the wine development tools package (Arch: wine-tools;
  Debian/Ubuntu: wine-development or libwine-dev), then re-run setup
  or re-run with --skip-verify to install without verification"

    # daemon question: the 0x12/0xA3 gates need a LIVE virtual DS4
    local daemon_up=0
    if pgrep -x ds4linux-daemon >/dev/null 2>&1; then
        say "daemon is already running — using it for verification"
        daemon_up=1
    else
        local answer=""
        if [[ $OPT_WITH_BUILD -eq 0 && -t 0 ]]; then
            printf 'Start the daemon now for verification? A connected DualSense is required. [Y/n] '
            read -r answer
        else
            warn "non-interactive run — daemon will NOT be started"
        fi
        if [[ "${answer,,}" == "y" || -z "$answer" && -t 0 ]]; then
            sudo_run ds4ctl start && daemon_up=1 || warn "ds4ctl start failed — continuing without the daemon"
        fi
    fi

    if [[ $daemon_up -ne 1 ]]; then
        warn "no live daemon: the 0x12/0xA3 gates CANNOT pass — marking unverified instead of failing"
        manifest_set "verified" "false"
        manifest_set "verify_reason" '"no-daemon"'
        warn "installed but NOT verified: connect a DualSense, run 'sudo ds4ctl start', then re-run setup (or verify manually: verifiers/verify.sh \"$DS_DIR\")"
        return
    fi

    # throwaway prefix under $HOME (wine refuses /tmp dirs in some setups)
    local pfx rc=0
    pfx="$(mktemp -d "$HOME/.proton-ds-verify.XXXXXX")" || die "cannot create a throwaway verify prefix under $HOME"
    say "throwaway WINEPREFIX: $pfx"
    # PATH: make the -DS dist's wine/wineserver win over any system wine —
    # otherwise a system wineserver with a different protocol version
    # intercepts the client (classic "version mismatch" wine client error)
    if ! (cd "$verifiers_dir" && PATH="$DS_DIR/files/bin:$PATH" WINEDEBUG=-all WINEPREFIX="$pfx" \
              wine wineboot -i >/dev/null 2>&1); then
        rm -rf "$pfx"
        die "failed to initialize the throwaway WINEPREFIX (wineboot)
  what to do: check disk space and the instance integrity; report if it persists"
    fi
    if ! (cd "$verifiers_dir" && PATH="$DS_DIR/files/bin:$PATH" ./verify.sh "$DS_DIR" "$pfx"); then
        rc=1
    fi
    rm -rf "$pfx"
    if [[ $rc -ne 0 ]]; then
        die "VERIFY GATE FAILED
  the verifier output above shows which gate failed
  safe abort: the -DS copy will be removed (trap); the stock instance is untouched
  note: the daemon and ds4ctl are installed system-wide — run uninstall.sh to remove them"
    fi
    say "verify gate: ALL GREEN"
    manifest_set "verified" "true"
    manifest_set "verify_reason" '"all-green (hidprobe + ditest)"'
    manifest_add_step "\"verify: ALL GREEN (hidprobe gates + ditest twin-hidden)\""
}

verify_gate

CREATED_DS=0
if [[ $DRY_RUN -eq 1 ]]; then
    say "DRY-RUN complete: action plan printed, no changes made"
else
    say "setup complete (current implementation level): $DS_DIR"
fi
