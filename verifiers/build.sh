#!/usr/bin/env bash
# Build the gameless verifiers with winegcc (host wine dev package needed:
# winegcc + /usr/include/wine/windows/ddk headers).
set -euo pipefail
cd "$(dirname "$0")"
INC="-I/usr/include/wine/windows -I/usr/include/wine/windows/ddk"
for t in hidpaths hidprobe ditest; do
    winegcc -o "$t.exe" "$t.c" $INC -lsetupapi -lhid $([ $t = ditest ] && echo -ldinput8 -lxinput -luuid -ldxguid) \
        || echo "FAIL: $t (check winegcc + wine headers)" >&2
done
ls -la ./*.exe 2>/dev/null || true
