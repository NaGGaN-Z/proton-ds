#!/usr/bin/env bash
# proton-ds gadget-shim build (T2: single TU; T3+ appends hid_core/ds_translate)
set -euo pipefail
cd "$(dirname "$0")"

SOURCES=(gadget_shim.cpp hid_core.cpp ds_translate.cpp pad_input.cpp bridge.cpp)
OUT=gadget-shim

CXX="${CXX:-g++}"
$CXX -std=c++20 -O2 -Wall -Wextra -o "$OUT" "${SOURCES[@]}"
echo "built: $(pwd)/$OUT"
