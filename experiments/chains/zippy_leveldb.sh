#!/bin/bash
# LevelDB on Facebook's FAST'20 ZippyDB model (M9, section 2): 50M records,
# 43-byte values, 256 MB cache, 86/14 get/put at 60k ops/s for 300 s.
#   STAGE=all trains the model on seed 42 (once); STAGE=matrix reuses it.
#   POLICIES defaults to the M9 rows; CONTROLS=1 adds the dry run.
# DB on the local NVMe: LEAPER_DB_ROOT (default /tmp/leaper_dbs).
set -eu
cd "$(dirname "$0")/../.."
ROOT=${LEAPER_DB_ROOT:-/tmp/leaper_dbs}; mkdir -p "$ROOT"
TAG=${TAG:-m4zippy}; MODEL_TAG=${MODEL_TAG:-$TAG}
W="NUM_KEYS=50000000 VALUE_SIZE=43 CACHE_MB=256 WRITE_BUFFER_MB=4 READ_DELAY_US=0 KEY_DIST=mixgraph READ_RATIO=0.86 UPDATE_RATIO=0.14 OP_RATE=${OP_RATE:-60000} WRITE_RATE=0 DURATION=300 RANGE_SIZE=2000"
env LEAPER_DB=$ROOT/zippy_ldb TAG=$TAG MODEL_TAG=$MODEL_TAG STAGE=${STAGE:-all} EVAL_SEED=${EVAL_SEED:-1234} $W \
  EXTRA_ARGS="--sine_a=1000 --sine_d=4500" \
  POLICIES="${POLICIES:-off eager_evict incremental_warmup warm_all warm_flush leaper_p2only}" ./experiments/run_m4.sh
if [ "${CONTROLS:-0}" = 1 ]; then
  env LEAPER_DB=$ROOT/zippy_ldb TAG=${TAG}_dry MODEL_TAG=$MODEL_TAG STAGE=matrix EVAL_SEED=${EVAL_SEED:-1234} $W \
    EXTRA_ARGS="--sine_a=1000 --sine_d=4500" LEAPER_EXTRA="--leaper_dry_run=1" POLICIES="leaper_p2only" ./experiments/run_m4.sh
fi
