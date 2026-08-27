#!/usr/bin/env bash
# M7: RocksDB evaluation.
#
# The comparison that matters on RocksDB is not "can the block cache be warmed"
# -- RocksDB ships that -- but "does choosing what to warm beat warming
# everything". So the matrix is:
#
#   off                   prepopulate_block_cache = kDisable  (stock)
#   flush_only            kFlushOnly            (RocksDB's default-available)
#   flush_and_compaction  kFlushAndCompaction   (unconditional, both sides)
#   leaper                selective, both sides, same core as the LevelDB port
#
# Same protocol as M4: train on seed 42, evaluate on seed 1234.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./build/leaper_bench_rocksdb
PY=.venv/bin/python
DB=${LEAPER_DB:-/tmp/leaper_m7_db}
OUT=${LEAPER_OUT:-experiments/results}
DUR=${DURATION:-300}
TAG=${TAG:-m7}
STAGE=${STAGE:-all}
mkdir -p "$OUT"

RANGE=40000
SLOT=1.0
WARMUP=30

WORKLOAD=(
  --num=4000000 --value_size=100 --cache_mb=128 --write_buffer_mb=8
  --max_file_mb=4 --block_kb=4
  --key_dist=lifecycle --life_range_size=$RANGE --life_hot_slots=16
  --life_lifetime_s=8 --life_ramp_frac=0.25 --life_chain=4 --life_chain_lag=0.2
  --threads=4 --read_ratio=0.75 --update_ratio=0.20
  --op_rate=40000 --write_rate=4000
  --duration="$DUR" --warmup=$WARMUP
)

if [ "$STAGE" = "all" ]; then
  echo "=== 1/3 training run (seed 42) ==="
  "$BIN" --db="$DB" "${WORKLOAD[@]}" --seed=42 --fill=1 --policy=off \
         --trace_out="$OUT/${TAG}_train" --out_prefix="$OUT/${TAG}_train"

  echo "=== 2/3 train models + calibrate ==="
  $PY tools/train_leaper.py --trace="$OUT/${TAG}_train" --slot_s=$SLOT \
      --range_size=$RANGE --steps=6 --out="$OUT/${TAG}.model" | tail -12
  $PY tools/calibrate_phases.py "$OUT/${TAG}_train" --block_kb=4 --cache_mb=128 \
      | tee "$OUT/${TAG}.calibration.txt"
fi
ALPHA=$(grep -o 'leaper_t1_alpha=[0-9.e+-]*' "$OUT/${TAG}.calibration.txt" | cut -d= -f2)
BETA=$(grep -o 'leaper_t2_beta=[0-9.e+-]*' "$OUT/${TAG}.calibration.txt" | cut -d= -f2)
echo "calibrated alpha=$ALPHA beta=$BETA"

echo "=== 3/3 policy matrix (seed 1234) ==="
for POL in off flush_only flush_and_compaction leaper; do
  echo "--- $POL ---"
  EXTRA=()
  if [ "$POL" = "leaper" ]; then
    EXTRA=(--model_prefix="$OUT/${TAG}.model" --model_steps=6
           --precursors="$OUT/${TAG}.model.precursors.txt"
           --leaper_range_size=$RANGE --leaper_slot_s=$SLOT
           --leaper_t1_alpha="$ALPHA" --leaper_t2_beta="$BETA")
  fi
  # Same reset as M4: the workload inserts new keys, so back-to-back policy
  # runs against one database would grow it monotonically and bias whichever
  # policy runs last.
  "$BIN" --db="$DB" "${WORKLOAD[@]}" --seed=1234 --fill=1 --policy="$POL" \
         ${EXTRA[@]+"${EXTRA[@]}"} --out_prefix="$OUT/${TAG}_$POL"
done

$PY tools/summarize_matrix.py "$OUT" "$TAG"
