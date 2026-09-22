#!/bin/bash
# Acceptance for M9 follow-up item F3: make_oracle.py must place a read that
# the trace stamps at t ms into plug-in slot floor((t + warmup_ms) / slot_ms),
# for any slot width, including one that does not divide the warmup.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
.venv/bin/python - "$T" <<'PY'
import struct, sys
t = sys.argv[1]
with open(f"{t}/tr.trace.0", "wb") as f:      # records: uint32 t_ms, uint32 key<<2|op (op 0 = read)
    for ms in (0, 5000): f.write(struct.pack("<II", ms, (0 << 2) | 0))
PY
check() {  # slot_s, expected slots
  .venv/bin/python tools/make_oracle.py --trace="$T/tr" --range_size=1000 --slot_s="$1" --warmup_s=30 --out="$T/o.txt" > /dev/null
  got=$(grep -v '^#' "$T/o.txt" | cut -d' ' -f1 | tr '\n' ' ' | sed 's/ $//')
  [ "$got" = "$2" ] && echo "  slot_s=$1: slots $got" || { echo "FAIL slot_s=$1: got '$got', want '$2'"; exit 1; }
}
check 1 "30 35"
check 5 "6 7"
check 7 "4 5"
echo "check_oracle_offset: PASS"
