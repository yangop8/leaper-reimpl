#!/usr/bin/env bash
# M1: capture access traces for the offline pipeline.
#
# Two workloads, because they exercise different halves of the paper's feature
# vector and a stationary zipfian stream exercises neither:
#
#   drift  the hot region advances 4000 keys/s, so ranges heat up and cool down
#          continuously. This is what the read/write arrival rate features are
#          for, and it is where the paper's "hot last slot => hot next slot"
#          baseline is systematically late.
#   phase  the hot region rotates between 4 regions every 60s (240s cycle).
#          Only a periodic workload gives the timestamp features any signal;
#          the paper's own ablation shows they contribute least to recall, and
#          on a non-periodic workload they should contribute nothing.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./build/leaper_bench
DB=${LEAPER_DB:-/tmp/leaper_m1_db}
OUT=experiments/results
mkdir -p "$OUT"

# op_rate keeps the trace to a size numpy can hold: 50k ops/s * 900s * 8 bytes
# is about 360 MB. Compaction cadence is still set by --write_rate.
COMMON=(
  --num=16000000 --value_size=100 --cache_mb=512 --write_buffer_mb=16
  --max_file_mb=8 --block_kb=4 --zipf=0.99 --key_dist=zipf
  --threads=8 --read_ratio=0.75 --update_ratio=0.20
  --op_rate=50000 --write_rate=8000 --duration=900 --warmup=30
)

echo "=== drift: hot region advances 4000 keys/s ==="
"$BIN" --db="$DB" "${COMMON[@]}" --hotspot_shift=4000 --fill=1 \
       --trace_out="$OUT/m1_drift" --out_prefix="$OUT/m1_drift"

echo "=== phase: 4 hot regions, 60s each ==="
"$BIN" --db="$DB" "${COMMON[@]}" --phases=4 --phase_period_s=60 --fill=0 \
       --trace_out="$OUT/m1_phase" --out_prefix="$OUT/m1_phase"
