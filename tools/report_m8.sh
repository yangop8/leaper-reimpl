#!/usr/bin/env bash
# One-shot summary of every review follow-up experiment, in both the harness's
# terms (summarize_matrix) and the paper's (paper_metrics).
cd "$(dirname "$0")/.."
PY=.venv/bin/python
OUT=experiments/results
sec() { echo; echo "################ $* ################"; }

sec "E1 RocksDB, warm-at-end, with row cache (m7v2)"
$PY tools/summarize_matrix.py $OUT m7v2 | grep -v missing

sec "E2 unthrottled overhead, NVMe (m4_ovh) -- the paper's 0.95%"
$PY tools/summarize_matrix.py $OUT m4_ovh | grep -v missing
$PY tools/paper_metrics.py $OUT m4_ovh --t2=2 | sed -n '3,12p'
echo "-- with collector sampling P=0.01 (m4_ovh_s01):"
$PY tools/summarize_matrix.py $OUT m4_ovh_s01 2>/dev/null | grep "Leaper"

sec "E3 oracle lookahead on slow storage (m4_slow_oracle_w*)"
for W in 1 5 20; do
  f=$OUT/m4_slow_oracle_w$W.timeseries.csv
  [ -f "$f" ] && $PY - "$f" "$W" <<'PYEOF'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
h = sum(float(r["block_hits"]) for r in rows); l = sum(float(r["block_lookups"]) for r in rows)
print(f"  oracle W={sys.argv[2]:>2}: hit ratio {100*h/l:.2f}%")
PYEOF
done
echo "  (reference from M4 slow: LRU 35.83%, WarmAll 41.09%, Leaper prefetch-only 37.28%)"

sec "E4 zipf skew sweep, slow storage (paper Fig. 13a)"
printf "%-6s %-10s %-10s %-10s\n" zipf LRU WarmAll Leaper
for Z in 0.0 0.3 0.5 0.9 1.0; do
  $PY - $OUT m4_zipf$Z $Z <<'PYEOF'
import csv, os, sys
out, tag, z = sys.argv[1], sys.argv[2], sys.argv[3]
def hr(p):
    f = os.path.join(out, f"{tag}_{p}.timeseries.csv")
    if not os.path.exists(f): return None
    rows = list(csv.DictReader(open(f)))
    h = sum(float(r["block_hits"]) for r in rows); l = sum(float(r["block_lookups"]) for r in rows)
    return 100*h/l if l else 0
v = [hr(p) for p in ("off","warm_all","leaper_p2only")]
print(f"{z:<6} " + " ".join(f"{x:9.2f}%" if x is not None else "   (miss) " for x in v))
PYEOF
done

sec "E5 range queries: 10% of reads are scans (m4_scan)"
$PY tools/summarize_matrix.py $OUT m4_scan | grep -v missing
$PY tools/paper_metrics.py $OUT m4_scan --t2=2 | sed -n '5,12p'

sec "E6 distribution shift (model: 16 slots/8s; workload: 64 slots/3s), with SSAD (m4_shift)"
$PY tools/summarize_matrix.py $OUT m4_shift | grep -v missing
grep -h "ssad_suspensions" $OUT/../../.. 2>/dev/null | head -0

################ G: re-measurement with compaction-path prefetch working ################
sec "G1 M4 slow storage v2 (paper's regime)"
$PY tools/summarize_matrix.py $OUT m4_slow_v2 | grep -v missing
$PY tools/paper_metrics.py $OUT m4_slow_v2 --t2=2 | sed -n '3,12p'
sec "G2 M4 NVMe v2"
$PY tools/summarize_matrix.py $OUT m4_nvme_v2 | grep -v missing
$PY tools/paper_metrics.py $OUT m4_nvme_v2 --t2=2 | sed -n '3,12p'
sec "G3 oracle lookahead v2"
for W in 1 5 20; do
  f=$OUT/m4_slow_v2_oracle_w$W.timeseries.csv
  [ -f "$f" ] && awk -F, -v w=$W 'NR>1{h+=$6;l+=$5;pi+=0} END{printf "  oracle W=%2s: hit ratio %.2f%%\n", w, 100*h/l}' "$f"
done
sec "G4 zipf sweep v2"
printf "%-6s %-10s %-10s %-10s\n" zipf LRU WarmAll Leaper
for Z in 0.0 0.3 0.5 0.9 0.99; do
  printf "%-6s " $Z
  for P in off warm_all leaper_p2only; do
    f=$OUT/m4_zipf${Z}_v2_$P.timeseries.csv
    [ -f "$f" ] && awk -F, 'NR>1{h+=$6;l+=$5} END{printf "%9.2f%% ", 100*h/l}' "$f" || printf "  (miss)   "
  done; echo
done
sec "G5 SSAD against a mid-run change at t=120s (m4_drift)"
$PY tools/summarize_matrix.py $OUT m4_drift | grep -v missing
for P in leaper_p2only leaper_p2only_ssad; do
  f=$OUT/m4_drift_$P.timeseries.csv
  [ -f "$f" ] && awk -F, -v p=$P 'NR>1 && $1<=120{h1+=$6;l1+=$5} NR>1 && $1>120{h2+=$6;l2+=$5;s+=$24} END{printf "  %-20s before shift %.2f%%  after shift %.2f%%  suspended %d s\n", p, 100*h1/l1, 100*h2/l2, s}' "$f"
done
sec "G6 scans v2"
$PY tools/summarize_matrix.py $OUT m4_scan_v2 | grep -v missing
$PY tools/paper_metrics.py $OUT m4_scan_v2 --t2=2 | sed -n '5,12p'

# ---------------------------------------------------------------------------
# H: the corrected measurements (compaction-path warming working, workload
# threads only in the hit ratio). Everything above is superseded by these.
# ---------------------------------------------------------------------------
echo; echo "=============== H: corrected measurements (v3) ==============="
sec "H1 slow storage, 64 MB (m4_slow_v3)"
$PY tools/summarize_matrix.py $OUT m4_slow_v3 | grep -v missing
$PY tools/paper_metrics.py $OUT m4_slow_v3 --t2=2 | sed -n '3,14p'
sec "H2 NVMe, 128 MB (m4_nvme_v3)"
$PY tools/summarize_matrix.py $OUT m4_nvme_v3 | grep -v missing
$PY tools/paper_metrics.py $OUT m4_nvme_v3 --t2=2 | sed -n '3,14p'
sec "H3 oracle lookahead, slow storage"
for W in 1 5 20; do
  f=$OUT/m4_slow_v3_oracle_w$W.timeseries.csv
  [ -f "$f" ] && awk -F, -v w=$W 'NR>1{h+=$6;l+=$5;c+=$13} END{printf "  oracle W=%2s: hit ratio %.2f%%  compactions %d\n", w, 100*h/l, c}' "$f"
done
sec "H4 zipf sweep, slow storage"
printf "%-6s %-10s %-10s %-10s\n" zipf LRU WarmAll Leaper
for Z in 0.0 0.3 0.5 0.9 0.99; do
  printf "%-6s " $Z
  for P in off warm_all leaper_p2only; do
    f=$OUT/m4_zipf${Z}_v3_$P.timeseries.csv
    [ -f "$f" ] && awk -F, 'NR>1{h+=$6;l+=$5} END{printf "%9.2f%% ", 100*h/l}' "$f" || printf "  (miss)   "
  done; echo
done
sec "H5/H7 shift at t=120s and SSAD (m4_drift_v3, relative 0.3; _ssad01: 0.1)"
$PY tools/summarize_matrix.py $OUT m4_drift_v3 | grep -v missing
for F in m4_drift_v3_off m4_drift_v3_warm_all m4_drift_v3_leaper_p2only m4_drift_v3_leaper_p2only_ssad m4_drift_v3_ssad01_leaper_p2only_ssad; do
  f=$OUT/$F.timeseries.csv
  [ -f "$f" ] && awk -F, -v p=$F 'NR>1 && $1<=120{h1+=$6;l1+=$5} NR>1 && $1>120{h2+=$6;l2+=$5;s+=$24} END{printf "  %-42s before %.2f%%  after %.2f%%  suspended %d s\n", p, 100*h1/l1, 100*h2/l2, s}' "$f"
done
sec "H6 scans, slow storage (m4_scan_v3)"
$PY tools/summarize_matrix.py $OUT m4_scan_v3 | grep -v missing
$PY tools/paper_metrics.py $OUT m4_scan_v3 --t2=2 | sed -n '5,10p'
sec "H8 same-seed repeats (m4_slow_v3r, m4_nvme_v3r)"
for T in m4_slow_v3r m4_nvme_v3r; do $PY tools/summarize_matrix.py $OUT $T 2>/dev/null | grep -E "LRU|WarmAll|Leaper \(prefetch only"; done
sec "H10 cache size: slow 128/256 MB, NVMe 64 MB"
for T in m4_slow128_v3 m4_slow256_v3 m4_nvme64_v3; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T | grep -v missing | sed -n '3,7p'; $PY tools/paper_metrics.py $OUT $T --t2=2 | sed -n '5,9p'; done
sec "H11 warm from a separate thread (m4_slow_v3a, m4_zipf0.99_v3a)"
for T in m4_slow_v3a m4_zipf0.99_v3a; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T 2>/dev/null | grep -v missing | sed -n '3,8p'; done
sec "H12/H13 RocksDB (m7v3; scan 40000/12000 keys per range)"
$PY tools/summarize_matrix.py $OUT m7v3 | grep -v missing
for K in 40000 12000; do f=$OUT/m7v3_scan${K}_leaper.timeseries.csv; [ -f "$f" ] && awk -F, -v k=$K 'NR>1{h+=$6;l+=$5} END{printf "  leaper, warm_scan_keys=%s: hit ratio %.2f%%\n", k, 100*h/l}' "$f"; done
sec "H14 hot lifetime: slow 128 MB / 40 s, NVMe 128 MB / 2 s"
for T in m4_slow128_life40 m4_nvme_life2; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T 2>/dev/null | grep -v missing | sed -n '3,7p'; done
sec "H15 RocksDB on LevelDB's tree shape (level_base_mb=10): m7v5, incl. block-level sst warming"
$PY tools/summarize_matrix.py $OUT m7v5 | grep -v missing
for K in off leaper sst_leaper; do f=$OUT/m7v5_$K.timeseries.csv; [ -f "$f" ] && awk -F, -v k=$K 'NR>1{h+=$6;l+=$5} END{printf "  %-12s background lookups %s (hits %s)\n", k, $27, $28}' "$f"; done
sec "H16 RocksDB, classic leveling (dynamic_level_bytes=0): m7v6"
$PY tools/summarize_matrix.py $OUT m7v6 | grep -v missing
sec "H17 RocksDB at the paper's scale (10 GB data, 3 GB cache): m7paper2 = churning hot set, m7paper3 = stable, m7paper3r = repeat"
for T in m7paper2 m7paper3 m7paper3r; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T | grep -v missing | sed -n '3,7p'; done
sec "H18 the paper's two real workloads, stationary (A/B = 10 GB table, C/D = the tables' own sizes)"
for T in m7zipf09 m7zipf03 m7fit_im m7fit_ec; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T | grep -v missing | sed -n '3,9p'; done
sec "I  after the 2026-09-19 review fixes (_v4 tags, models retrained)"
for T in m4_slow_v4 m4_nvme_v4 m4_slow128_life40_v4; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T 2>/dev/null | grep -v missing | sed -n '3,11p'; $PY tools/paper_metrics.py $OUT $T --t2=2 2>/dev/null | sed -n '5,13p'; done
for T in m7paper3_v4 m7zipf09_v4 m7fit_im_v4; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T 2>/dev/null | grep -v missing | sed -n '3,7p'; done
sec "I' RocksDB after the 2026-09-20 follow-up (End under the adapter lock), _v5"
for T in m7paper3_v5 m7zipf09_v5 m7fit_im_v5; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT $T 2>/dev/null | grep -v missing | sed -n '3,7p'; done
echo; echo "=============== M9: journal prep ==============="
sec "M9.1 Leaper through RocksDB's patched prepopulate path (_v6) next to the _v5 rows"
for T in m7paper3 m7zipf09 m7fit_im; do echo "-- $T"; $PY tools/summarize_matrix.py $OUT ${T}_v5 2>/dev/null | grep -v missing | sed -n '3,7p'; f=$OUT/${T}_v6_prepop_leaper.timeseries.csv; [ -f "$f" ] && awk -F, 'NR>1{h+=$6;l+=$5} END{printf "%-22s %8.2f%%  (prepopulate, _v6)\n", "Leaper (prepopulate)", 100*h/l}' "$f"; done
sec "M9.2 FAST'20 ZippyDB model (mixgraph): RocksDB m7zippy, LevelDB m4zippy"
$PY tools/summarize_matrix.py $OUT m7zippy 2>/dev/null | grep -v missing | sed -n '3,8p'
$PY tools/summarize_matrix.py $OUT m4zippy 2>/dev/null | grep -v missing | sed -n '3,9p'
echo "-- LevelDB controls (docs/M9-journal-prep.md, section 2): async warm, one step, 100k ranges, dry run, v9 (memo + tick monitor)"
for T in m4zippy_async m4zippy_step1 m4zippy_r100k m4zippy_dry m4zippy_v9; do $PY tools/summarize_matrix.py $OUT $T 2>/dev/null | grep -v missing | sed -n '3,9p' | sed "s/^/  [$T] /"; done

sec "M9.3 Multi-seed variance: four evaluation seeds per cell (1235-1238), models fixed, paired margins vs the first policy"
echo "-- LevelDB ZippyDB model"; $PY tools/seed_stats.py $OUT off,warm_flush,warm_all,leaper_p2only m4zippy_s1235 m4zippy_s1236 m4zippy_s1237 m4zippy_s1238 | sed -n '3,8p'
echo "-- RocksDB paper scale"; $PY tools/seed_stats.py $OUT off,flush_only,flush_and_compaction,prepop_leaper m7paper3_s1235 m7paper3_s1236 m7paper3_s1237 m7paper3_s1238 | sed -n '3,8p'
echo "-- RocksDB IM on 10 GB"; $PY tools/seed_stats.py $OUT off,flush_and_compaction,prepop_leaper m7zipf09_s1235 m7zipf09_s1236 m7zipf09_s1237 m7zipf09_s1238 | sed -n '3,7p'
echo "-- RocksDB IM at 8m rows"; $PY tools/seed_stats.py $OUT off,flush_and_compaction,prepop_leaper m7fit_im_s1235 m7fit_im_s1236 m7fit_im_s1237 m7fit_im_s1238 | sed -n '3,7p'
echo "-- RocksDB ZippyDB model"; $PY tools/seed_stats.py $OUT off,flush_and_compaction,prepop_leaper m7zippy_s1235 m7zippy_s1236 m7zippy_s1237 m7zippy_s1238 | sed -n '3,7p'
echo "-- LevelDB NVMe 128 MB (H2)"; $PY tools/seed_stats.py $OUT off,eager_evict,warm_all,leaper_p2only m4_nvme_s1235 m4_nvme_s1236 m4_nvme_s1237 m4_nvme_s1238 | sed -n '3,8p'
