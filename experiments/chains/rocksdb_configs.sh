#!/bin/bash
# The four RocksDB configurations of M8/M9 (3 GB cache unless stated):
#   paper   paper scale, 50M x 184 B, lifecycle hot set of 60 s (H17)
#   im10g   IM shape, zipf 0.9 on the same 10 GB table (H18)
#   im8m    IM shape at its own 8m-row size (H18)
#   zippy   FAST'20 ZippyDB model, 50M x 43 B, 256 MB cache (M9)
#   CONFIGS="paper im10g" selects; STAGE=all trains once per config, then
#   STAGE=matrix reuses (MODEL_TAG_SUFFIX names the trained tag, e.g. _v5).
# POLICIES default: stock, the two built-ins, Leaper through the patched
# prepopulate path. EXTRA (e.g. "--direct_reads=1") is appended to every run.
set -eu
cd "$(dirname "$0")/../.."
ROOT=${LEAPER_DB_ROOT:-/tmp/leaper_dbs}; mkdir -p "$ROOT"
# "-" not ":-": an explicitly empty MODEL_TAG_SUFFIX means "the unsuffixed
# model" (repeat runs write to _rN and reuse it), only an unset one follows TAG.
SUF=${TAG_SUFFIX:-}; MSUF=${MODEL_TAG_SUFFIX-$SUF}
RUN_M7=${RUN_M7:-./experiments/run_m7.sh}
P=${POLICIES:-"off flush_only flush_and_compaction prepop_leaper"}
RDB="NUM_KEYS=50000000 VALUE_SIZE=184 CACHE_MB=3072 WRITE_BUFFER_MB=16 L0_TRIGGER=4 LEVEL_BASE_MB=256 MAX_FILE_MB=64 OP_RATE=${OP_RATE:-60000} DURATION=200"
IM="KEY_DIST=zipf ZIPF=0.9 READ_RATIO=0.40 UPDATE_RATIO=0.60 WRITE_RATE=0 RANGE_SIZE=2000"
run(){ env LEAPER_DB=$ROOT/$1 TAG=$2$SUF MODEL_TAG=$2$MSUF STAGE=${STAGE:-all} EVAL_SEED=${EVAL_SEED:-1234} POLICIES="$P" EXTRA_ARGS="${EXTRA:-}" "${@:3}" "$RUN_M7"; }
for C in ${CONFIGS:-paper im10g im8m zippy}; do
  case $C in
    paper) run m7paper_db m7paper3 $RDB RANGE_SIZE=100000 WRITE_RATE=15000 LIFETIME_S=60 ;;
    im10g) run m7paper_db m7zipf09 $RDB $IM ;;
    im8m)  run m7fit_db m7fit_im $RDB $IM NUM_KEYS=8000000 ;;
    zippy) run zippy_db m7zippy NUM_KEYS=50000000 VALUE_SIZE=43 CACHE_MB=256 WRITE_BUFFER_MB=4 L0_TRIGGER=4 LEVEL_BASE_MB=256 MAX_FILE_MB=64 RANGE_SIZE=2000 OP_RATE=${OP_RATE:-60000} WRITE_RATE=0 SINE_A=1000 SINE_D=4500 DURATION=300 KEY_DIST=mixgraph READ_RATIO=0.86 UPDATE_RATIO=0.14 ;;
    *) echo "unknown config $C" >&2; exit 2 ;;
  esac
done
