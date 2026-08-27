#!/usr/bin/env bash
# Fetch the vendored engines and apply the LevelDB hook patch.
#
# LevelDB is a git submodule pinned to release 1.23 and is *not* forked: the
# hooks Leaper needs live in a patch file so that what was changed in the engine
# stays reviewable as a diff rather than disappearing into a fork.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> submodules"
git submodule update --init third_party/leveldb

PATCH=adapters/leveldb/leveldb-1.23-leaper-hooks.patch
echo "==> applying $PATCH"
if git -C third_party/leveldb apply --check --reverse "../../$PATCH" 2>/dev/null; then
  echo "    already applied"
else
  git -C third_party/leveldb apply "../../$PATCH"
  echo "    applied ($(grep -c '^+++' "$PATCH") files)"
fi

echo
echo "==> build (LevelDB half)"
echo "    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
echo "    cmake --build build -j"
echo
echo "==> build (RocksDB half, optional; takes a while)"
echo "    git submodule update --init third_party/rocksdb"
echo "    cmake -S third_party/rocksdb -B build-rocksdb -G Ninja -DCMAKE_BUILD_TYPE=Release \\"
echo "          -DWITH_GFLAGS=0 -DWITH_TESTS=0 -DWITH_TOOLS=0 -DROCKSDB_BUILD_SHARED=0"
echo "    cmake --build build-rocksdb --target rocksdb -j"
echo "    cmake -S . -B build -G Ninja && cmake --build build -j"
