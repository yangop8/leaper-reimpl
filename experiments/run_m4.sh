#!/usr/bin/env bash
# M4: the baseline matrix.
#
# Protocol, in order, because the ordering is what makes the comparison valid:
#
#   1. TRAIN run   seed 42,  policy=off, trace captured -> models + precursors
#   2. calibrate   T1/T2 constants fitted to that run's own compaction log
#   3. EVAL trace  seed 1234, policy=off, trace captured -> oracle hot sets
#   4. EVAL runs   seed 1234, one per policy, identical in every other respect
#
# Training and evaluation use different seeds so the model never sees the
# workload realisation it is scored on. The oracle is built from the evaluation
# trace on purpose: it is the upper bound, and is supposed to know the future.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./build/leaper_bench
PY=.venv/bin/python
DB=${LEAPER_DB:-/tmp/leaper_m4_db}
OUT=${LEAPER_OUT:-experiments/results}
DELAY=${READ_DELAY_US:-0}
DUR=${DURATION:-300}
OPRATE=${OP_RATE:-40000}
WRATE=${WRITE_RATE:-4000}
CACHEMB=${CACHE_MB:-128}
NTHREADS=${THREADS:-4}
TAG=${TAG:-m4}
MODEL_TAG=${MODEL_TAG:-$TAG}
# STAGE=matrix reuses an existing model/oracle and reruns only the policy runs.
STAGE=${STAGE:-all}
mkdir -p "$OUT"

RANGE=${RANGE_SIZE:-40000}
SLOT=1.0
WARMUP=30

WORKLOAD=(
  --num=${NUM_KEYS:-4000000} --value_size=100 --cache_mb="$CACHEMB" --write_buffer_mb=8
  --max_file_mb=4 --block_kb=4
  --key_dist=lifecycle --life_range_size=$RANGE --life_hot_slots=${HOT_SLOTS:-16}
  --life_lifetime_s=8 --life_ramp_frac=0.25 --life_chain=4 --life_chain_lag=0.2
  --threads="$NTHREADS" --read_ratio=${READ_RATIO:-0.75} --update_ratio=${UPDATE_RATIO:-0.20}
  --write_corr=${WRITE_CORR:-1.0}
  --op_rate="$OPRATE" --write_rate="$WRATE"
  --duration="$DUR" --warmup=$WARMUP --read_delay_us="$DELAY"
)

if [ "$STAGE" = "all" ]; then
echo "=== 1/4 training run (seed 42) ==="
"$BIN" --db="$DB" "${WORKLOAD[@]}" --seed=42 --fill=1 --policy=off \
       --trace_out="$OUT/${TAG}_train" --out_prefix="$OUT/${TAG}_train"

echo "=== 2/4 train models + calibrate phases ==="
$PY tools/train_leaper.py --trace="$OUT/${TAG}_train" --slot_s=$SLOT \
    --range_size=$RANGE --steps=6 --out="$OUT/${MODEL_TAG}.model" | tail -20
$PY tools/calibrate_phases.py "$OUT/${TAG}_train" --block_kb=4 --cache_mb="$CACHEMB" \
    | tee "$OUT/${MODEL_TAG}.calibration.txt"
ALPHA=$(grep -o 'leaper_t1_alpha=[0-9.e+-]*' "$OUT/${MODEL_TAG}.calibration.txt" | cut -d= -f2)
BETA=$(grep -o 'leaper_t2_beta=[0-9.e+-]*' "$OUT/${MODEL_TAG}.calibration.txt" | cut -d= -f2)
echo "calibrated alpha=$ALPHA beta=$BETA"

echo "=== 3/4 evaluation trace (seed 1234) -> oracle ==="
"$BIN" --db="$DB" "${WORKLOAD[@]}" --seed=1234 --fill=0 --policy=off \
       --trace_out="$OUT/${TAG}_eval" --out_prefix="$OUT/${TAG}_off"
$PY tools/make_oracle.py --trace="$OUT/${TAG}_eval" --range_size=$RANGE \
    --slot_s=$SLOT --slot_offset=$WARMUP --out="$OUT/${MODEL_TAG}.oracle.txt"

else
  echo "=== stages 1-3 skipped (STAGE=$STAGE) ==="
  ALPHA=$(grep -o 'leaper_t1_alpha=[0-9.e+-]*' "$OUT/${MODEL_TAG}.calibration.txt" | cut -d= -f2)
  BETA=$(grep -o 'leaper_t2_beta=[0-9.e+-]*' "$OUT/${MODEL_TAG}.calibration.txt" | cut -d= -f2)
fi

echo "=== 4/4 policy matrix (seed 1234) ==="
LEAPER_ARGS=(
  --leaper_range_size=$RANGE --leaper_slot_s=$SLOT
  --leaper_t1_alpha="$ALPHA" --leaper_t2_beta="$BETA"
)
# leaper_p2only is Leaper with the eviction phase disabled. It is a separate
# row because the two phases move the hit ratio in opposite directions here,
# and reporting only their sum would hide that.
for POL in ${POLICIES:-off eager_evict incremental_warmup warm_all leaper leaper_p2only oracle}; do
  echo "--- $POL ---"
  EXTRA=()
  RUNPOL="$POL"
  case "$POL" in
    leaper)
      EXTRA=(--model_prefix="$OUT/${MODEL_TAG}.model" --model_steps=6
             --precursors="$OUT/${MODEL_TAG}.model.precursors.txt") ;;
    leaper_p2only)
      RUNPOL=leaper
      EXTRA=(--model_prefix="$OUT/${MODEL_TAG}.model" --model_steps=6
             --precursors="$OUT/${MODEL_TAG}.model.precursors.txt"
             --leaper_phase1=0) ;;
    oracle)
      EXTRA=(--oracle="$OUT/${MODEL_TAG}.oracle.txt") ;;
  esac
  # macOS ships bash 3.2, where "${EXTRA[@]}" on an empty array trips set -u.
  # Every policy starts from the same database. The workload inserts new keys
  # (5% of operations), so running the policies back to back against one DB
  # grows it by ~260k keys per run -- 46% over seven policies, monotonically,
  # which biases whichever policy happens to run last.
  "$BIN" --db="$DB" "${WORKLOAD[@]}" --seed=1234 --fill=1 --policy="$RUNPOL" \
         "${LEAPER_ARGS[@]}" ${EXTRA[@]+"${EXTRA[@]}"} --out_prefix="$OUT/${TAG}_$POL"
done

$PY tools/summarize_matrix.py "$OUT" "$TAG"
