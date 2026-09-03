#!/usr/bin/env bash
# M7 at the paper's scale.
#
# H15/H16 found the RocksDB null result to be a matter of how much compaction
# there is to recover from: on a 480 MB database RocksDB rewrites ~0.5 GB per
# 300 s against LevelDB's 9.6 GB. The Leaper paper's online experiments run on
# 10 GB of data with a 3 GB block cache (30%) and 200-byte records, where
# X-Engine has six levels and each background operation moves a lot of hot
# data -- "about 10 background operations" in a 200 s test (Section 7.3).
#
# This script puts RocksDB in that shape: 50M records x 200 B = 10 GB, a 3 GB
# block cache, RocksDB's own level sizing, and 200 s measured. Everything else
# (protocol, policies, models) is run_m7.sh's.
#
# Cost: ~15 min to fill, then ~4 min per policy, ~1 h in total, and it writes
# tens of GB. Run it plugged in.
set -euo pipefail
cd "$(dirname "$0")/.."

NUM_KEYS=${NUM_KEYS:-50000000} \
VALUE_SIZE=${VALUE_SIZE:-184} \
CACHE_MB=${CACHE_MB:-3072} \
WRITE_BUFFER_MB=${WRITE_BUFFER_MB:-64} \
MAX_FILE_MB=${MAX_FILE_MB:-64} \
LEVEL_BASE_MB=${LEVEL_BASE_MB:-256} \
RANGE_SIZE=${RANGE_SIZE:-250000} \
OP_RATE=${OP_RATE:-40000} \
WRITE_RATE=${WRITE_RATE:-4000} \
DURATION=${DURATION:-200} \
TAG=${TAG:-m7paper} \
POLICIES="${POLICIES:-off flush_only flush_and_compaction sst_leaper}" \
  ./experiments/run_m7.sh
