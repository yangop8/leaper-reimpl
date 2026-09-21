#!/bin/bash
# Run a command only once the machine is quiet: 1-minute load below LOAD_MAX
# (default 3) and no process above 50% CPU other than the benches, for three
# checks a minute apart. Then log the load once a minute while it runs.
# Lesson from 2026-09-19: a matrix measured alongside another job at load 20
# had 10x the stock tail latency and half the compactions; it was discarded.
#   experiments/chains/gate.sh experiments/chains/seeds.sh
set -u
LOAD_MAX=${LOAD_MAX:-3.0}
stamp(){ echo "[$(date +%H:%M:%S)] $*"; }
loadavg(){ if [ -r /proc/loadavg ]; then cut -d' ' -f1 /proc/loadavg; else sysctl -n vm.loadavg | awk '{print $2}' | tr ',' '.'; fi; }
hogs(){ ps -Ao %cpu,comm | awk 'NR>1 && $1>50 && $2 !~ /leaper_bench|gate.sh/' | wc -l | tr -d ' '; }
quiet=0
while [ $quiet -lt 3 ]; do
  l=$(loadavg); h=$(hogs)
  if awk -v l="$l" -v m="$LOAD_MAX" 'BEGIN{exit !(l<m)}' && [ "$h" -eq 0 ]; then quiet=$((quiet+1)); else quiet=0; fi
  stamp "load=$l cpu_hogs=$h quiet_checks=$quiet"
  [ $quiet -lt 3 ] && sleep 60
done
stamp "machine quiet; starting: $*"
LOG=${LOAD_LOG:-experiments/results/load_$(date +%Y%m%d_%H%M%S).log}
( while true; do echo "$(date +%H:%M:%S) load=$(loadavg) hogs=$(hogs)"; sleep 60; done ) > "$LOG" 2>&1 &
MON=$!
"$@"; rc=$?
kill $MON 2>/dev/null
stamp "done rc=$rc; load log: $LOG (max load $(awk -F'load=' '{split($2,a," "); if(a[1]+0>m)m=a[1]+0} END{print m}' "$LOG"))"
exit $rc
