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

RANGE=${RANGE_SIZE:-40000}
SLOT=1.0
WARMUP=30

# LSM geometry. RocksDB's default L1 of 256 MB gives this database five
# compactions in five minutes; LevelDB's hard-coded 10 MB gives ~250. The
# engine comparison only means something on the same shape of tree, so the
# default here is LevelDB's; LEVEL_BASE_MB=256 reproduces the pre-H15 runs.
WORKLOAD=(
  --num=${NUM_KEYS:-4000000} --value_size=${VALUE_SIZE:-100}
  --cache_mb=${CACHE_MB:-128} --write_buffer_mb=${WRITE_BUFFER_MB:-8}
  --max_file_mb=${MAX_FILE_MB:-4} --block_kb=4
  --level_base_mb=${LEVEL_BASE_MB:-10} --l0_trigger=${L0_TRIGGER:-4}
  --key_dist=lifecycle --life_range_size=$RANGE --life_hot_slots=${HOT_SLOTS:-16}
  --life_lifetime_s=${LIFETIME_S:-8} --life_ramp_frac=0.25 --life_chain=4 --life_chain_lag=0.2
  --threads=4 --read_ratio=0.75 --update_ratio=0.20
  --op_rate=${OP_RATE:-40000} --write_rate=${WRITE_RATE:-4000}
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
for POL in ${POLICIES:-off flush_only flush_and_compaction leaper sst_leaper leaper_rowcache}; do
  echo "--- $POL ---"
  EXTRA=()
  RUNPOL="$POL"
  case "$POL" in
    leaper)
      EXTRA=(--model_prefix="$OUT/${TAG}.model" --model_steps=6
             --precursors="$OUT/${TAG}.model.precursors.txt"
             --leaper_range_size=$RANGE --leaper_slot_s=$SLOT
             --leaper_t1_alpha="$ALPHA" --leaper_t2_beta="$BETA") ;;
    sst_leaper)
      # Block-level warming of the job's own output files through an
      # SstFileReader that shares the DB's block cache (see sst_warm_check).
      RUNPOL=leaper
      EXTRA=(--model_prefix="$OUT/${TAG}.model" --model_steps=6
             --precursors="$OUT/${TAG}.model.precursors.txt"
             --leaper_range_size=$RANGE --leaper_slot_s=$SLOT
             --leaper_t1_alpha="$ALPHA" --leaper_t2_beta="$BETA"
             --warm_mode=sst) ;;
    leaper_rowcache)
      # The paper's rows go to a KV cache; RocksDB has one. Same block cache
      # budget plus a 32 MB row cache, to see whether the prefetcher's
      # range-granularity warming helps the row cache more than the block cache.
      RUNPOL=leaper
      EXTRA=(--model_prefix="$OUT/${TAG}.model" --model_steps=6
             --precursors="$OUT/${TAG}.model.precursors.txt"
             --leaper_range_size=$RANGE --leaper_slot_s=$SLOT
             --leaper_t1_alpha="$ALPHA" --leaper_t2_beta="$BETA"
             --row_cache_mb=32) ;;
  esac
  # Same reset as M4: the workload inserts new keys, so back-to-back policy
  # runs against one database would grow it monotonically and bias whichever
  # policy runs last.
  "$BIN" --db="$DB" "${WORKLOAD[@]}" --seed=1234 --fill=1 --policy="$RUNPOL" \
         ${EXTRA[@]+"${EXTRA[@]}"} --out_prefix="$OUT/${TAG}_$POL"
done

$PY tools/summarize_matrix.py "$OUT" "$TAG"
