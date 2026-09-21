#!/bin/bash
# Acceptance for M9 review item R1 (docs/code-review-m9-2026-09-21.md): a
# STAGE=matrix rerun of run_m4.sh under a new TAG/EVAL_SEED must (a) make its
# own oracle when the oracle policy is requested and none exists for that TAG,
# (b) refuse an oracle whose recorded identity does not match, and (c) reuse
# the model for non-oracle policies without touching any oracle. Drives the
# real script with a stub bench that only echoes its arguments.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/out"
printf '#!/bin/bash\necho "STUB $*"\n' > "$T/stub.sh"; chmod +x "$T/stub.sh"
ln -s "$PWD/tools" "$T/tools"; ln -s "$PWD/build" "$T/build"; ln -s "$PWD/.venv" "$T/.venv"
sed 's#cd "$(dirname "$0")/.."#cd "$(dirname "$0")"#' experiments/run_m4.sh > "$T/run_m4.sh"; chmod +x "$T/run_m4.sh"
echo 'leaper_t1_alpha=1e-5 leaper_t2_beta=100' > "$T/out/trained.calibration.txt"
run() { env LEAPER_BIN="$T/stub.sh" LEAPER_OUT=out MODEL_TAG=trained STAGE=matrix "$@" "$T/run_m4.sh"; }
fail=0
# (a) no oracle for this TAG: the script must trace this seed to make one.
if ! (run TAG=t_s1235 EVAL_SEED=1235 POLICIES=oracle 2>&1 || true) | grep -q -- "--seed=1235 --fill=0 --policy=off --trace_out=out/t_s1235_eval"; then
  echo "FAIL (a): missing oracle was not generated from the evaluation seed"; fail=1
fi
# (b) an oracle with another identity must be refused, exit 3.
echo "seed=1234 stale" > "$T/out/t_s1235.oracle.meta"; : > "$T/out/t_s1235.oracle.txt"
set +e; run TAG=t_s1235 EVAL_SEED=1235 POLICIES=oracle > "$T/b.log" 2>&1; rc=$?; set -e
if [ "$rc" != 3 ] || ! grep -q "made for different traffic" "$T/b.log"; then
  echo "FAIL (b): mismatched oracle identity was not refused (rc=$rc)"; fail=1
fi
# (c) non-oracle policies reuse the model; no oracle argument anywhere.
out=$(run TAG=t_s1236 EVAL_SEED=1236 POLICIES="off leaper_p2only" 2>&1 || true)
if ! echo "$out" | grep -q -- "--seed=1236 --fill=1 --policy=leaper .*--model_prefix=out/trained.model" || echo "$out" | grep -q -- "--oracle="; then
  echo "FAIL (c): non-oracle matrix did not reuse the model cleanly"; fail=1
fi
[ "$fail" = 0 ] && echo "check_run_m4_oracle_binding: PASS" || exit 1
