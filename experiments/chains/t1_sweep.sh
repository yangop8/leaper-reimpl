#!/bin/bash
# Phase-1 decision (M9, section 5): the H14 configuration (slow storage
# emulated at 200 us per read, 128 MB cache, 40 s hot lifetimes) with the SST
# size swept so that a compaction runs ~2, ~6 and ~28 s. Trains per size.
#   SIZES="4 16 64"; STEPS (default 24); SLOT_S (default 1; use 5 for 64 MB).
set -eu
cd "$(dirname "$0")/../.."
ROOT=${LEAPER_DB_ROOT:-/tmp/leaper_dbs}; mkdir -p "$ROOT"
for F in ${SIZES:-4 16 64}; do
  env LEAPER_DB=$ROOT/m4slow_db TAG=m4_t1_f$F${TAG_SUFFIX:-} STAGE=all DURATION=180 LIFETIME_S=40 \
    READ_DELAY_US=200 OP_RATE=8000 WRITE_RATE=1500 CACHE_MB=128 MAX_FILE_MB=$F STEPS=${STEPS:-24} SLOT_S=${SLOT_S:-1.0} \
    POLICIES="${POLICIES:-off eager_evict leaper_p1only leaper_p2only leaper}" ./experiments/run_m4.sh
done
