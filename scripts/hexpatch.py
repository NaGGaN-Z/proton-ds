#!/usr/bin/env python3
"""proton-ds hex-patch engine.

Ассерты и порядок операций — 1:1 из рецептов:
  patches/winexinput-swap.md  (IG_00 <-> XI_00, UTF-16LE)
  patches/hidclass-guid.md    (GUID_DEVINTERFACE_WINEXINPUT, last byte +1)

Команды:
  swap <file> [--dry-run]   — обменять "&IG_00" <-> "&XI_00" (winexinput.sys)
  guid <file> [--dry-run]   — GUID -> FAKE (hidclass.sys)
  check <file> [--kind swap|guid] — детект состояния (stock/patched/drift)
  selftest [--fixtures DIR] — золотой тест на tests/fixtures

Бэкап оригинала: <file>.pdsbak (создаётся один раз; перезапись refused).
Выход: 0 ок; 1 провал ассерта/мисматч; 2 usage/IO; 10 check=patched; 11 check=drift.
"""
import argparse
import hashlib
import os
import shutil
import sys
import tempfile

IG = b"&\x00I\x00G\x00_\x000\x000\x00"
XI = b"&\x00X\x00I\x00_\x000\x000\x00"
PLACEHOLDER = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44])
GUID = bytes.fromhex("fdd5536c80640f44b618476750c5e1a6")
FAKE = bytes.fromhex("fdd5536c80640f44b618476750c5e1a7")
BACKUP_SUFFIX = ".pdsbak"

LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()


def log(msg):
    if LOG_LEVEL in ("INFO", "DEBUG", "WARN", "ERROR"):
        print(f"[hexpatch] {msg}", flush=True)


def dbg(msg):
    if LOG_LEVEL == "DEBUG":
        print(f"[hexpatch:debug] {msg}", flush=True)


def die(code, msg):
    print(f"[hexpatch] ERROR: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        die(2, f"cannot read {path}: {e}")


def write_file(path, data):
    try:
        with open(path, "wb") as f:
            f.write(data)
    except OSError as e:
        die(2, f"cannot write {path}: {e}")


def do_swap_bytes(data, path):
    n_ig, n_xi = data.count(IG), data.count(XI)
    dbg(f"{path}: IG count={n_ig} @ {[hex(i) for i in offsets(data, IG)]}, "
        f"XI count={n_xi} @ {[hex(i) for i in offsets(data, XI)]}")
    if n_ig != 1:
        die(1, f"unsupported driver binary: '&IG_00' occurs {n_ig} times, expected exactly 1\n"
               f"  file: {path}\n"
               f"  likely cause: this is not a stock GE-Proton/Proton winexinput.sys "
               f"(different GE/Wine version, or already modified by another tool)\n"
               f"  safe abort: the file was NOT modified")
    if n_xi != 1:
        die(1, f"unsupported driver binary: '&XI_00' occurs {n_xi} times, expected exactly 1\n"
               f"  file: {path}\n"
               f"  likely cause: this is not a stock GE-Proton/Proton winexinput.sys "
               f"(different GE/Wine version, or already modified by another tool)\n"
               f"  safe abort: the file was NOT modified")
    d = data.replace(IG, PLACEHOLDER)
    d = d.replace(XI, IG)
    d = d.replace(PLACEHOLDER, XI)
    if not (d.count(IG) == 1 and d.count(XI) == 1 and len(d) == len(data)):
        die(1, f"internal error: post-swap verification failed for {path}\n"
               f"  (IG={d.count(IG)}, XI={d.count(XI)}, size {len(d)} vs {len(data)})\n"
               f"  this is a bug in hexpatch, please report it\n"
               f"  safe abort: the file was NOT modified")
    return d


def do_guid_bytes(data, path):
    n = data.count(GUID)
    dbg(f"{path}: GUID count={n} @ {[hex(i) for i in offsets(data, GUID)]}")
    if n < 1:
        die(1, f"unsupported driver binary: WINEXINPUT interface GUID not found\n"
               f"  file: {path}\n"
               f"  likely cause: this is not a stock GE-Proton/Proton hidclass.sys "
               f"(different GE/Wine version, or already modified by another tool)\n"
               f"  safe abort: the file was NOT modified")
    return data.replace(GUID, FAKE)


def offsets(data, pat):
    out, i = [], data.find(pat)
    while i != -1:
        out.append(i)
        i = data.find(pat, i + 1)
    return out


def infer_kind(path):
    base = os.path.basename(path).lower()
    if "winexinput" in base:
        return "swap"
    if "hidclass" in base:
        return "guid"
    return None


def make_backup(path, dry_run):
    backup = path + BACKUP_SUFFIX
    if os.path.exists(backup):
        die(1, f"backup {backup} already exists — {os.path.basename(path)} looks patched already\n"
               f"  re-patching now would double-apply the patch\n"
               f"  if you really want to re-patch the current state, remove the backup file "
               f"manually first (this loses the restore point)")
    if dry_run:
        log(f"[dry-run] backup: cp {path} -> {backup}")
        return
    shutil.copy2(path, backup)
    log(f"backup created: {backup} ({os.path.getsize(backup)} B)")


def apply_patch(path, kind, dry_run):
    data = read_file(path)
    log(f"patch ({kind}): {path} ({len(data)} B, sha256 {sha256(data)[:12]}...)")
    if kind == "swap":
        d = do_swap_bytes(data, path)
        ia, ib = data.find(IG), data.find(XI)
        log(f"swap: IG@{hex(ia)} <-> XI@{hex(ib)}, equal length, count 1/1 before and after — ok")
    else:
        d = do_guid_bytes(data, path)
        log(f"guid: {data.count(GUID)} occurrence(s) -> FAKE (last byte +1)")
    if dry_run:
        log(f"[dry-run] no write performed; resulting sha256 would be {sha256(d)[:12]}...")
        return
    make_backup(path, dry_run=False)
    write_file(path, d)
    log(f"written: {path} (sha256 {sha256(d)})")


def check_state(path, kind):
    if kind is None:
        die(2, f"cannot determine patch kind from filename {path}; pass --kind swap|guid")
    data = read_file(path)
    log(f"check ({kind}): {path} ({len(data)} B)")
    if kind == "swap":
        n_ig, n_xi = data.count(IG), data.count(XI)
        if not (n_ig == 1 and n_xi == 1):
            die(11, f"cannot classify {path}: '&IG_00' x{n_ig}, '&XI_00' x{n_xi}, expected 1/1\n"
                    f"  the file was modified by something other than proton-ds\n"
                    f"  what to do: restore it from the .pdsbak/.swbak backup next to it, "
                    f"or reinstall the Proton instance")
        for suffix in (BACKUP_SUFFIX, ".swbak"):
            backup = path + suffix
            if os.path.exists(backup):
                b = read_file(backup)
                if (data.find(IG) == b.find(XI) and data.find(XI) == b.find(IG)
                        and b.count(IG) == 1 and b.count(XI) == 1):
                    log(f"verdict: PATCHED (offset comparison vs {os.path.basename(backup)})")
                    sys.exit(10)
                if data.find(IG) == b.find(IG) and data.find(XI) == b.find(XI):
                    log(f"verdict: STOCK (matches {os.path.basename(backup)})")
                    sys.exit(0)
                die(11, f"cannot classify {path}: offsets match neither stock nor swapped layout "
                        f"(vs {backup})\n"
                        f"  the file was modified by something other than proton-ds\n"
                        f"  what to do: restore it from that backup, or reinstall the Proton instance")
        log("no backup found — using offset-order heuristic (less reliable):")
        if data.find(IG) < data.find(XI):
            log(f"verdict: STOCK? (IG@{hex(data.find(IG))} < XI@{hex(data.find(XI))} — stock order)")
            sys.exit(0)
        log(f"verdict: PATCHED? (IG@{hex(data.find(IG))} > XI@{hex(data.find(XI))} — swapped order)")
        sys.exit(10)
    n_guid, n_fake = data.count(GUID), data.count(FAKE)
    if n_fake > 0 and n_guid == 0:
        log(f"verdict: PATCHED (FAKE x{n_fake})")
        sys.exit(10)
    if n_guid > 0 and n_fake == 0:
        log(f"verdict: STOCK (GUID x{n_guid})")
        sys.exit(0)
    die(11, f"cannot classify {path}: GUID x{n_guid}, FAKE x{n_fake}\n"
            f"  the file contains both (or neither) — it was modified by something else\n"
            f"  what to do: restore it from the .pdsbak/.gdbak backup, or reinstall the Proton instance")


def parse_golden(path):
    golden = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            sha, target, state = parts[0], parts[1], parts[2]
            golden.setdefault(target, {})[state] = sha
    return golden


def selftest(fixtures_dir, golden_path):
    if not os.path.isdir(fixtures_dir):
        die(2, f"fixtures directory not found: {fixtures_dir}\n"
               f"  what to do: run from the proton-ds repo, or pass --fixtures DIR")
    if not os.path.exists(golden_path):
        die(2, f"golden hash file not found: {golden_path}")
    golden = parse_golden(golden_path)
    log(f"golden set: {len(golden)} targets from {golden_path}")
    targets = [
        ("x86_64-windows/winexinput.sys", "swap"),
        ("x86_64-windows/hidclass.sys", "guid"),
        ("i386-windows/winexinput.sys", "swap"),
        ("i386-windows/hidclass.sys", "guid"),
    ]
    results = []
    for rel, kind in targets:
        path = os.path.join(fixtures_dir, rel)
        if not os.path.exists(path):
            results.append((rel, "MISSING", "fixture file not found"))
            continue
        data = read_file(path)
        entry = golden.get(rel, {})
        stock_sha = entry.get("stock")
        patched_sha = entry.get("patched")
        if stock_sha and sha256(data) != stock_sha:
            results.append((rel, "FAIL", f"stock hash mismatch vs golden "
                                         f"({sha256(data)[:12]}... != {stock_sha[:12]}...)"))
            continue
        if kind == "swap":
            d = do_swap_bytes(data, path)
        else:
            d = do_guid_bytes(data, path)
        got = sha256(d)
        if patched_sha and got != patched_sha:
            results.append((rel, "FAIL", f"patched hash mismatch vs golden "
                                         f"({got[:12]}... != {patched_sha[:12]}...)"))
        else:
            results.append((rel, "PASS", f"golden hash matched ({got[:12]}...)"))
    neg = negative_assert_test(fixtures_dir, targets)
    results.append(neg)
    print()
    ok = True
    for rel, status, detail in results:
        mark = {"PASS": "ok", "FAIL": "FAIL", "MISSING": "FAIL"}.get(status, "FAIL")
        if status != "PASS":
            ok = False
        print(f"  [{mark}] {rel:<38} {status}: {detail}")
    print()
    if ok:
        log(f"SELFTEST ALL GREEN ({len(results)}/{len(results)})")
        sys.exit(0)
    die(1, f"SELFTEST FAILED ({sum(1 for r in results if r[1] != 'PASS')}/{len(results)} failures)\n"
           f"  the engine does not reproduce the verified deployed state\n"
           f"  what to do: do not use this build; report it (engine or fixtures drifted)")


def negative_assert_test(fixtures_dir, targets):
    rel, kind = targets[0]
    src = os.path.join(fixtures_dir, rel)
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "winexinput.sys")
        data = bytearray(read_file(src))
        ia = data.find(IG)
        data[ia:ia + len(IG)] = b"\x00" * len(IG)
        write_file(probe, bytes(data))
        before = read_file(probe)
        rc = run_cli(["swap", probe, "--dry-run"])
        after = read_file(probe)
        if rc != 1:
            return ("negative-assert", "FAIL", f"expected exit 1 on zero-match, got {rc}")
        if before != after:
            return ("negative-assert", "FAIL", "file was modified despite failed assertion")
        return ("negative-assert", "PASS", "zero-match -> exit 1, file untouched")


def run_cli(argv):
    import subprocess
    return subprocess.run([sys.executable, os.path.abspath(__file__)] + argv,
                          capture_output=True).returncode


def main():
    p = argparse.ArgumentParser(description="proton-ds hex-patch engine (see module docstring)")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("swap", "guid"):
        s = sub.add_parser(name)
        s.add_argument("file")
        s.add_argument("--dry-run", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("file")
    c.add_argument("--kind", choices=["swap", "guid"])
    st = sub.add_parser("selftest")
    st.add_argument("--fixtures", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures"))
    st.add_argument("--golden", default=None)
    args = p.parse_args()
    if args.cmd == "swap":
        apply_patch(args.file, "swap", args.dry_run)
    elif args.cmd == "guid":
        apply_patch(args.file, "guid", args.dry_run)
    elif args.cmd == "check":
        check_state(args.file, args.kind or infer_kind(args.file))
    elif args.cmd == "selftest":
        golden = args.golden or os.path.join(args.fixtures, "golden-hashes.txt")
        selftest(os.path.abspath(args.fixtures), golden)
    sys.exit(0)


if __name__ == "__main__":
    main()
