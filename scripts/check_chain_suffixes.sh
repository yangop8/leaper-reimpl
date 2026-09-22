#!/bin/bash
# Acceptance for M9 follow-up item F2: in experiments/chains/rocksdb_configs.sh
# an explicitly empty MODEL_TAG_SUFFIX means "reuse the unsuffixed model"
# (repeat runs), an unset one follows TAG_SUFFIX (a fresh training run), and
# a non-empty one names the trained tag. Drives the chain with a stub run_m7.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
printf '#!/bin/bash\necho "TAG=$TAG MODEL_TAG=$MODEL_TAG STAGE=$STAGE"\n' > "$T/stub.sh"; chmod +x "$T/stub.sh"
want() { got=$(env "$@" RUN_M7="$T/stub.sh" CONFIGS=paper LEAPER_DB_ROOT="$T" ./experiments/chains/rocksdb_configs.sh); case "$got" in *"$EXPECT"*) echo "  ok: $got";; *) echo "FAIL: $* -> $got (want $EXPECT)"; exit 1;; esac; }
EXPECT="TAG=m7paper3_r1 MODEL_TAG=m7paper3 STAGE=matrix"    want TAG_SUFFIX=_r1 MODEL_TAG_SUFFIX= STAGE=matrix
EXPECT="TAG=m7paper3_r1 MODEL_TAG=m7paper3_r1 STAGE=all"    want TAG_SUFFIX=_r1 STAGE=all
EXPECT="TAG=m7paper3_s1235 MODEL_TAG=m7paper3_v5 STAGE=matrix" want TAG_SUFFIX=_s1235 MODEL_TAG_SUFFIX=_v5 STAGE=matrix
# repeat.sh composes the same way for every repeat.
got=$(env RUN_M7="$T/stub.sh" CONFIGS=paper LEAPER_DB_ROOT="$T" STAGE=matrix MODEL_TAG_SUFFIX= REPEATS=2 ./experiments/chains/repeat.sh ./experiments/chains/rocksdb_configs.sh | grep MODEL_TAG)
echo "$got" | grep -q "TAG=m7paper3_r1 MODEL_TAG=m7paper3 " && echo "$got" | grep -q "TAG=m7paper3_r2 MODEL_TAG=m7paper3 " || { echo "FAIL repeat: $got"; exit 1; }
echo "check_chain_suffixes: PASS"
