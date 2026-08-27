#!/usr/bin/env bash
# M0: does the cache invalidation problem reproduce on stock LevelDB?
#
# Three regimes on the same 16M-record (~1.9 GB) database with a 512 MB block
# cache (~27%, close to the paper's 3 GB / 10 GB):
#
#   A  8 threads, paced writes  -- read pressure high enough that LevelDB's
#                                  seek-triggered compactions run continuously
#   B  2 threads, paced writes  -- lower read rate, looking for quiet periods
#   C  2 threads, read-only     -- no memtable flushes at all; isolates
#                                  seek-triggered compaction as a source of
#                                  invalidation
#
# NOTE: flags are kept in a bash array, not a string. Under zsh an unquoted
# "$COMMON" is NOT word-split, so the whole string arrives as one argument and
# every flag after the first is silently swallowed into its value. The binary
# now rejects that, but arrays avoid the trap entirely.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./build/leaper_bench
DB=${LEAPER_DB:-/tmp/leaper_m0_db}
OUT=experiments/results
mkdir -p "$OUT"

COMMON=(
  --num=16000000 --value_size=100 --cache_mb=512 --write_buffer_mb=16
  --max_file_mb=8 --block_kb=4 --zipf=0.99 --key_dist=zipf
  --duration=300 --warmup=30 --stale_after=10
)

echo "=== A: 8 threads, 12k writes/s ==="
"$BIN" --db="$DB" "${COMMON[@]}" --threads=8 --read_ratio=0.75 --update_ratio=0.20 \
       --write_rate=12000 --fill=1 --out_prefix="$OUT/m0_A_8t"

echo "=== B: 2 threads, 12k writes/s ==="
"$BIN" --db="$DB" "${COMMON[@]}" --threads=2 --read_ratio=0.75 --update_ratio=0.20 \
       --write_rate=12000 --fill=0 --out_prefix="$OUT/m0_B_2t"

echo "=== C: 2 threads, read-only ==="
"$BIN" --db="$DB" "${COMMON[@]}" --threads=2 --read_ratio=1.0 --update_ratio=0.0 \
       --fill=0 --out_prefix="$OUT/m0_C_ro"

.venv/bin/python tools/plot_timeseries.py "$OUT/m0_A_8t" "$OUT/m0_B_2t" "$OUT/m0_C_ro"
