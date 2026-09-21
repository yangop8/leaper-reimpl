#!/bin/bash
# Same configuration, same seed, N times: the repeatability check for the
# tail-latency and QPS columns (M10). Wraps any chain; each repeat gets its
# own TAG_SUFFIX so nothing is overwritten.
#   REPEATS=3 experiments/chains/repeat.sh experiments/chains/rocksdb_configs.sh
set -eu
for i in $(seq 1 "${REPEATS:-3}"); do
  echo "=== repeat $i/${REPEATS:-3} ==="
  TAG_SUFFIX="${TAG_SUFFIX:-}_r$i" "$@"
done
