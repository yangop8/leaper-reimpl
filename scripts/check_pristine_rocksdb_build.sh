#!/bin/bash
# Acceptance for M9 review item R2 (docs/code-review-m9-2026-09-21.md): the
# RocksDB adapter and bench must still compile against a RocksDB 11.8 that
# does NOT carry adapters/rocksdb/rocksdb-11.8-prepopulate-filter.patch, with
# LEAPER_HAVE_PREPOP_FILTER undefined. Takes the pristine table.h from the
# submodule's HEAD (the working tree may be patched), puts it first on the
# include path, and syntax-checks both translation units.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/rocksdb"
git -C third_party/rocksdb show HEAD:include/rocksdb/table.h > "$T/rocksdb/table.h"
if grep -q PrepopulateBlockFilter "$T/rocksdb/table.h"; then
  echo "submodule HEAD already carries the patch; nothing to check against" >&2
  exit 2
fi
for f in adapters/rocksdb/leaper_rocksdb.cc bench/src/leaper_bench_rocksdb.cc; do
  c++ -std=c++17 -fsyntax-only -w -I"$T" -Ithird_party/rocksdb/include -Ileaper/include \
      -Iadapters/rocksdb -Ibench/include "$f"
  echo "OK without the patch: $f"
done
echo "check_pristine_rocksdb_build: PASS"
