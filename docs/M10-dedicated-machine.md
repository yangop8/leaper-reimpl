# M10 — The dedicated-machine measurements

This is a work order for a coding agent running on a rented machine with a
fresh clone of this repository. It says what machine to expect, how to
prepare it, what to run, in what order, and what to hand back. Everything
here is about the two columns that a laptop cannot measure — **overhead
(QPS cost) and tail latency** — and about **repeatability**. The hit-ratio
results are settled (README, `docs/M9-journal-prep.md`) and are re-measured
here only as a transfer check.

## 1. The machine

| | requirement | why |
|---|---|---|
| CPU | x86-64, **8 or more physical cores** (16 threads), dedicated — no burstable or shared-core instance | 4 workload threads, LevelDB's 1 and RocksDB's 2 background threads, the monitor thread, and headroom; neighbours on shared cores are the source of the unrepeatable p99 seen so far |
| RAM | 32 GB or more | 10 GB data set, 3 GB block cache, traces and models during training |
| storage | **local NVMe**, 200 GB or more, not network block storage | the LevelDB ZippyDB cell rewrites 78 GB per 300 s; the benches bypass the page cache so misses hit the device, and network volumes add millisecond jitter of their own |
| OS | Ubuntu 22.04 or 24.04 | gcc 11+ or clang 14+, cmake 3.16+, ninja, python3-venv |
| billing | by the hour or month; budget 2-3 days of machine time | the full plan below is about 20 hours of runs plus setup |

Examples that fit: a Hetzner dedicated server (AX41-NVMe class), an Alibaba
Cloud local-disk instance (ecs.i3/i4, 16 vCPU), AWS i4i.2xlarge or
c6id.4xlarge. Do not use t-series instances, instances with only cloud
disks, or ARM — the existing numbers are from Apple Silicon and the journal
version wants an x86 baseline.

Record at the start of every session, into `experiments/results/M10-machine.txt`:
`lscpu`, `free -g`, `lsblk -d -o NAME,MODEL,SIZE,ROTA`, the mount used for
`LEAPER_DB_ROOT`, `uname -a`, compiler versions, and `cat /proc/loadavg`.

## 2. Preparing the machine

```sh
sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git python3-venv python3-pip libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev
# Local NVMe for the databases (adjust the device):
sudo mkfs.ext4 -F /dev/nvme1n1 && sudo mkdir -p /mnt/nvme && sudo mount -o noatime /dev/nvme1n1 /mnt/nvme && sudo chown $USER /mnt/nvme
export LEAPER_DB_ROOT=/mnt/nvme/leaper_dbs
# Quiet the machine:
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor   # if the file exists
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
sudo systemctl disable --now unattended-upgrades apt-daily.timer apt-daily-upgrade.timer 2>/dev/null || true
ulimit -n 65536
```

Do not turn SMT off; it is not needed at 4 workload threads. Leave the
machine otherwise idle: no builds, no downloads, nothing else running while
a chain runs. `experiments/chains/gate.sh` refuses to start a chain until
the 1-minute load is below 3 and no other process is above 50% CPU for
three checks a minute apart, and logs the load once a minute while the
chain runs; every chain below goes through it.

## 3. Build and checks

```sh
git clone --recurse-submodules <repo> Leaper && cd Leaper
python3 -m venv .venv && .venv/bin/pip install lightgbm numpy
./scripts/setup.sh                                  # applies the LevelDB patch; RocksDB step is skipped until the submodule exists
git submodule update --init third_party/rocksdb
./scripts/setup.sh                                  # now applies the RocksDB prepopulate-filter patch
cmake -S third_party/rocksdb -B build-rocksdb -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_GFLAGS=0 -DWITH_TESTS=0 -DWITH_TOOLS=0 -DROCKSDB_BUILD_SHARED=0
cmake --build build-rocksdb --target rocksdb -j     # 10-20 minutes
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # must print "RocksDB prepopulate filter patch: present"
cmake --build build -j
./build/leaper/mapper_check && ./build/leaper/core_check && ./build/mixgraph_check | tail -3
./build/sst_warm_check /tmp/swc && ./build/budget_check /tmp/bc
./scripts/check_pristine_rocksdb_build.sh && ./scripts/check_prepop_detection.sh && ./scripts/check_run_m4_oracle_binding.sh && ./scripts/check_oracle_offset.sh && ./scripts/check_chain_suffixes.sh
```

Every check must print PASS before any experiment. `pread_env.h` has a
Linux path (`posix_fadvise(DONTNEED)` after each read) that has not been
exercised on Linux before this; confirm in T0 that the LevelDB stock run's
read p50 is tens of microseconds, not single digits — single digits mean
the page cache is serving misses.

## 4. Ground rules for the agent

* One chain at a time, always through `experiments/chains/gate.sh`.
* Never edit a script while a chain that uses it is running; never edit
  anything under `leaper/` (the core) — this milestone measures, it does not
  redesign. Harness fixes (bench, scripts, tools) are allowed and must be
  committed with a message saying what changed and why.
* Results go to `experiments/results/` as the scripts write them (the CSV,
  meta and calibration files are committed; traces, models, oracles and
  engine logs are gitignored). Commit after each test section on a branch
  `m10-results`, with the load log of that section. Push.
* Read the numbers before moving on. If a run's QPS column is not at its
  budget, or a policy's compaction count differs from stock's by more than
  a third, stop and say so in the report; do not "fix" it by rerunning.
* The summary tools: `tools/summarize_matrix.py OUT TAG` (one matrix),
  `tools/seed_stats.py OUT POLICIES TAG1 TAG2 ...` (mean, sd, min, max and
  paired margins across seeds or repeats, with the read p99 spread),
  `tools/paper_metrics.py OUT TAG` (prefetch precision, in-window and
  out-of-window hit ratios, spikes), `tools/report_m8.sh` (the whole
  report; extend its M9 sections with an M10 section for these runs).

## 5. Tests

### T0 — smoke, 20 minutes

One short run per engine to confirm the Linux paths:

```sh
LEAPER_DB_ROOT=$LEAPER_DB_ROOT ./build/leaper_bench --db=$LEAPER_DB_ROOT/smoke --num=1000000 --value_size=100 --cache_mb=32 --duration=60 --warmup=10 --op_rate=20000 --fill=1 --policy=off --out_prefix=experiments/results/m10_smoke_ldb
./build/leaper_bench_rocksdb --db=$LEAPER_DB_ROOT/smoke_rdb --num=1000000 --value_size=100 --cache_mb=32 --duration=60 --warmup=10 --op_rate=20000 --fill=1 --policy=off --direct_reads=1 --out_prefix=experiments/results/m10_smoke_rdb
```

Check: both exit 0; `read_p50_us` in the timeseries is 20-200 us (device
reads), the QPS column sits on the budget, and the hit ratio is not 100%.

### T1 — repeatability, ~6 hours

The question this machine exists to answer: is the tail latency
repeatable here? On the laptop the same RocksDB policy's mean per-second
p99 varied threefold between identical runs.

```sh
# RocksDB: train once per config, then three identical matrices. Direct reads on.
EXTRA="--direct_reads=1" CONFIGS="paper im10g" ./experiments/chains/gate.sh ./experiments/chains/rocksdb_configs.sh
EXTRA="--direct_reads=1" CONFIGS="paper im10g" STAGE=matrix MODEL_TAG_SUFFIX="" REPEATS=3 ./experiments/chains/gate.sh ./experiments/chains/repeat.sh ./experiments/chains/rocksdb_configs.sh
# LevelDB NVMe/128 MB (H2), same protocol:
env LEAPER_DB=$LEAPER_DB_ROOT/m4_db TAG=m4_nvme DURATION=300 READ_DELAY_US=0 OP_RATE=40000 WRITE_RATE=4000 CACHE_MB=128 POLICIES="off eager_evict warm_all leaper_p2only" ./experiments/chains/gate.sh ./experiments/run_m4.sh
for i in 1 2 3; do env LEAPER_DB=$LEAPER_DB_ROOT/m4_db TAG=m4_nvme_r$i MODEL_TAG=m4_nvme STAGE=matrix DURATION=300 READ_DELAY_US=0 OP_RATE=40000 WRITE_RATE=4000 CACHE_MB=128 POLICIES="off eager_evict warm_all leaper_p2only" ./experiments/chains/gate.sh ./experiments/run_m4.sh; done
# Summaries:
python3 tools/seed_stats.py experiments/results off,flush_only,flush_and_compaction,prepop_leaper m7paper3_r1 m7paper3_r2 m7paper3_r3
python3 tools/seed_stats.py experiments/results off,flush_only,flush_and_compaction,prepop_leaper m7zipf09_r1 m7zipf09_r2 m7zipf09_r3
python3 tools/seed_stats.py experiments/results off,eager_evict,warm_all,leaper_p2only m4_nvme_r1 m4_nvme_r2 m4_nvme_r3
```

Report, per policy: mean p99 across the three repeats, its sd and range,
and the same for p95 (from the CSVs) and hit ratio. Acceptance for calling
tail latency "measured": the p99 sd across repeats is below 10% of the mean
for stock. If it is not, say so — that is a result too — and include the
load logs.

### T2 — overhead, ~4 hours

The paper reports Leaper's overhead as the QPS lost outside background
operations. The rate-paced runs cannot show it (every policy sits on the
budget); unthrottled runs can.

```sh
# LevelDB ZippyDB, unthrottled (OP_RATE=0), stock vs prefetch-only vs warm everything vs the dry run:
TAG=m4zippy ./experiments/chains/gate.sh ./experiments/chains/zippy_leveldb.sh          # trains; seed 1234, paced (the transfer check of T4 needs it anyway)
for i in 1 2 3; do TAG=m4zippy_unth_r$i MODEL_TAG=m4zippy STAGE=matrix CONTROLS=1 POLICIES="off warm_all leaper_p2only" OP_RATE=0 ./experiments/chains/gate.sh ./experiments/chains/zippy_leveldb.sh; done
# RocksDB paper scale, unthrottled:
EXTRA="--direct_reads=1" CONFIGS=paper STAGE=matrix MODEL_TAG_SUFFIX="" OP_RATE=0 REPEATS=3 TAG_SUFFIX=_unth ./experiments/chains/gate.sh ./experiments/chains/repeat.sh ./experiments/chains/rocksdb_configs.sh
```

(`OP_RATE=0` is read by both run scripts as "no budget".) Report QPS per
policy relative to stock, with the three-repeat spread, and next to it the
`[leaper] inferences= inference_us=` line from each Leaper run's log and the
compaction count. For LevelDB the dry-run row separates the cost of
inference from the effect of warming. The overhead claim in the paper's
Table 5 is under 1%; state what it is here, with the spread.

### T3 — tail latency with device reads, ~4 hours

The four RocksDB configurations with `--direct_reads=1`, seed 1234, the
M9 policy set, and the M9 tables' hit ratios as the transfer check:

```sh
EXTRA="--direct_reads=1" ./experiments/chains/gate.sh ./experiments/chains/rocksdb_configs.sh     # STAGE=all for the two configs T1 did not train (im8m, zippy); reuse for paper, im10g via STAGE=matrix MODEL_TAG_SUFFIX=""
```

Report per config and policy: hit ratio (should match `docs/M9-journal-prep.md`
sections 1-2 and 4 within 0.5pp — if it does not, that is the first
finding), p95, p99, QPS. Then the same for the three LevelDB headline cells:

```sh
TAG=m4zippy STAGE=matrix CONTROLS=1 ./experiments/chains/gate.sh ./experiments/chains/zippy_leveldb.sh            # if T2 has not already produced it
./experiments/chains/gate.sh ./experiments/chains/t1_sweep.sh                                                    # SIZES="4 16 64"
SIZES=64 SLOT_S=5 STEPS=10 TAG_SUFFIX=_s5 ./experiments/chains/gate.sh ./experiments/chains/t1_sweep.sh
```

### T4 — transfer check of the hit-ratio conclusions (folded into T2/T3)

Compare every hit ratio measured above with the README tables. Expected:
the LevelDB ZippyDB cell around +11pp for Leaper and WarmAll negative; the
RocksDB IM-on-10 GB cell Leaper about 2pp over `kFlushAndCompaction`; the
8m-row cell the other way by about 3.7pp; the ZippyDB RocksDB cell all
warming policies within 0.1pp of each other; the paper-scale cell three
policies within a third of a point. A difference beyond 1pp in any cell is
the headline of the report, not something to explain away.

### T5 — if time remains: seeds

```sh
RDB_MODEL_SUFFIX="" NVME_MODEL_TAG=m4_nvme ./experiments/chains/gate.sh ./experiments/chains/seeds.sh    # ~8 hours; needs T2/T3's trained models
```

## 6. Deliverables

1. `experiments/results/M10-machine.txt` (section 1) and the load logs.
2. All result CSVs, committed on branch `m10-results`, one commit per test.
3. `docs/M10-results.md`: one table per test with the numbers above, the
   acceptance verdicts (T1's repeatability, T2's overhead with spread,
   T3/T4's transfer check), and a short list of anything that had to be
   changed in the harness to run on Linux, with the commit hashes. Written
   in English, numbers in tables, no narrative longer than the tables.
4. `./tools/report_m8.sh > experiments/results/M8-report.txt` regenerated
   with an M10 section added to the script.

## 7. Time budget

| section | machine time |
|---|---|
| setup, build, checks, T0 | 1 h |
| T1 repeatability | 6 h |
| T2 overhead | 4 h |
| T3 tail latency and LevelDB cells | 5 h |
| T5 seeds (optional) | 8 h |
| **total** | **16 h, 24 h with T5** |
