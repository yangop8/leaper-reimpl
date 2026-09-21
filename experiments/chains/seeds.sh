#!/bin/bash
# Multi-seed variance (M9, section 4): rerun the headline cells under
# evaluation seeds SEEDS (default 1235..1238) with the models fixed. Assumes
# the seed-1234 STAGE=all runs exist (zippy_leveldb.sh, rocksdb_configs.sh,
# nvme128 below) so their models and calibrations can be reused.
# Summarise with: tools/seed_stats.py experiments/results <policies> <tag>_s1235 <tag>_s1236 ...
set -eu
cd "$(dirname "$0")/../.."
ROOT=${LEAPER_DB_ROOT:-/tmp/leaper_dbs}
for SEED in ${SEEDS:-1235 1236 1237 1238}; do
  for C in ${CELLS:-zippy_ldb paper im10g im8m zippy nvme128}; do
    case $C in
      zippy_ldb) TAG=m4zippy_s$SEED MODEL_TAG=m4zippy STAGE=matrix EVAL_SEED=$SEED POLICIES="off warm_flush warm_all leaper_p2only" ./experiments/chains/zippy_leveldb.sh ;;
      nvme128)   env LEAPER_DB=$ROOT/m4_db TAG=m4_nvme_s$SEED MODEL_TAG=${NVME_MODEL_TAG:-m4_nvme_v4} STAGE=matrix EVAL_SEED=$SEED DURATION=300 READ_DELAY_US=0 OP_RATE=40000 WRITE_RATE=4000 CACHE_MB=128 POLICIES="off eager_evict warm_all leaper_p2only" ./experiments/run_m4.sh ;;
      paper|im10g|im8m|zippy) CONFIGS=$C TAG_SUFFIX=_s$SEED MODEL_TAG_SUFFIX=${RDB_MODEL_SUFFIX:-_v5} STAGE=matrix EVAL_SEED=$SEED POLICIES="${RDB_POLICIES:-off flush_and_compaction prepop_leaper}" ./experiments/chains/rocksdb_configs.sh ;;
    esac
  done
done
