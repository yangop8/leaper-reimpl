#!/bin/bash
# Acceptance for M9 follow-up item F1: the patch probe must follow the header,
# not the CMake cache. Configures a tiny project that includes the real
# cmake/DetectPrepopFilter.cmake three times in ONE build directory: against
# the submodule's unpatched table.h, then the patched tree, then unpatched
# again. Expects absent -> present -> absent.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/pristine/rocksdb" "$T/proj" "$T/build"
git -C third_party/rocksdb show HEAD:include/rocksdb/table.h > "$T/pristine/rocksdb/table.h"
grep -q PrepopulateBlockFilter "$T/pristine/rocksdb/table.h" && { echo "submodule HEAD already patched; cannot test" >&2; exit 2; }
grep -q PrepopulateBlockFilter third_party/rocksdb/include/rocksdb/table.h || { echo "working tree not patched; cannot test the present case" >&2; exit 2; }
cat > "$T/proj/CMakeLists.txt" <<CM
cmake_minimum_required(VERSION 3.16)
project(probe CXX)
set(ROCKSDB_INCLUDE "\${PROBE_INCLUDE}")
include($PWD/cmake/DetectPrepopFilter.cmake)
CM
probe() {  # $1 = label, $2 = include list (semicolon-separated), $3 = expected word
  out=$(cmake -S "$T/proj" -B "$T/build" -DPROBE_INCLUDE="$2" 2>&1 | grep "prepopulate filter patch" || true)
  case "$out" in *"$3"*) echo "  $1: $out";; *) echo "FAIL $1: expected '$3', got: $out"; exit 1;; esac
}
probe "unpatched (fresh)"     "$T/pristine;$PWD/third_party/rocksdb/include" absent
probe "patched (same dir)"    "$PWD/third_party/rocksdb/include"              present
probe "unpatched (same dir)"  "$T/pristine;$PWD/third_party/rocksdb/include" absent
echo "check_prepop_detection: PASS"
