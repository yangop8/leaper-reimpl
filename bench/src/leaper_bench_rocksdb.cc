// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// leaper_bench_rocksdb -- the M6/M7 harness.
//
// Same workload generators, same metrics and same output format as the LevelDB
// harness, so the two engines can be compared rather than just each described.
// Differences that are engine-specific and not choices:
//
//  * No StatsCache decorator is needed: RocksDB's own Statistics report
//    BLOCK_CACHE_DATA_HIT/MISS, which is what the LevelDB harness had to
//    build by hand.
//  * No PreadEnv is needed: RocksDB defaults to pread (allow_mmap_reads is
//    false), so unlike LevelDB it does not silently bypass its own block cache.
//  * The collector is driven from the client, because RocksDB has no read hook.
//    In a deployment it would live in DBImpl::GetImpl.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/write_batch.h"

#include "leaper_bench/histogram.h"
#include "leaper_bench/keygen.h"
#include "leaper_bench/trace.h"
#include "leaper_rocksdb.h"

namespace leaper_bench {
namespace {

struct Flags {
  std::string db = "/tmp/leaper_rocks_db";
  std::string out_prefix = "experiments/results/m7";
  uint64_t num = 4000000;
  int value_size = 100;
  int cache_mb = 128;
  int write_buffer_mb = 8;
  int max_file_mb = 4;
  int block_kb = 4;
  int threads = 4;
  double read_ratio = 0.75;
  double update_ratio = 0.20;
  double zipf = 0.99;
  std::string key_dist = "lifecycle";
  double hotspot = 0.0;
  uint64_t life_range_size = 40000;
  int life_hot_slots = 16;
  double life_lifetime_s = 8.0;
  double life_ramp_frac = 0.25;
  double life_cold_frac = 0.0;
  int life_chain = 4;
  double life_chain_lag = 0.2;
  double op_rate = 40000;
  double write_rate = 4000;
  int duration = 300;
  int warmup = 30;
  bool fill = true;
  uint64_t seed = 42;
  std::string trace_out;
  // off | flush_only | flush_and_compaction | leaper
  std::string policy = "off";
  std::string model_prefix;
  int model_steps = 6;
  std::string precursors;
  uint64_t leaper_range_size = 40000;
  double leaper_slot_s = 1.0;
  double leaper_t1_alpha = 2.77e-5;
  double leaper_t2_beta = 3709.0;
  double leaper_threshold = 0.5;
  int warm_scan_keys = 4096;
};

Flags flags;

bool ParseFlag(const char* arg, const char* name, std::string* out) {
  const size_t n = std::strlen(name);
  if (std::strncmp(arg, "--", 2) != 0) return false;
  if (std::strncmp(arg + 2, name, n) != 0 || arg[2 + n] != '=') return false;
  *out = arg + 3 + n;
  return true;
}
[[noreturn]] void BadValue(const char* name, const std::string& v) {
  std::fprintf(stderr, "invalid value for --%s: '%s'\n", name, v.c_str());
  std::exit(2);
}
uint64_t ParseU64(const char* n, const std::string& v) {
  char* e = nullptr;
  const uint64_t r = std::strtoull(v.c_str(), &e, 10);
  if (v.empty() || *e != '\0') BadValue(n, v);
  return r;
}
int ParseInt(const char* n, const std::string& v) {
  char* e = nullptr;
  const long r = std::strtol(v.c_str(), &e, 10);
  if (v.empty() || *e != '\0') BadValue(n, v);
  return static_cast<int>(r);
}
double ParseDouble(const char* n, const std::string& v) {
  char* e = nullptr;
  const double r = std::strtod(v.c_str(), &e);
  if (v.empty() || *e != '\0') BadValue(n, v);
  return r;
}
bool ParseBool(const char* n, const std::string& v) {
  if (v == "1" || v == "true") return true;
  if (v == "0" || v == "false") return false;
  BadValue(n, v);
}

void ParseArgs(int argc, char** argv) {
  std::string v;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (ParseFlag(a, "db", &v)) flags.db = v;
    else if (ParseFlag(a, "out_prefix", &v)) flags.out_prefix = v;
    else if (ParseFlag(a, "num", &v)) flags.num = ParseU64("num", v);
    else if (ParseFlag(a, "value_size", &v)) flags.value_size = ParseInt("value_size", v);
    else if (ParseFlag(a, "cache_mb", &v)) flags.cache_mb = ParseInt("cache_mb", v);
    else if (ParseFlag(a, "write_buffer_mb", &v)) flags.write_buffer_mb = ParseInt("write_buffer_mb", v);
    else if (ParseFlag(a, "max_file_mb", &v)) flags.max_file_mb = ParseInt("max_file_mb", v);
    else if (ParseFlag(a, "block_kb", &v)) flags.block_kb = ParseInt("block_kb", v);
    else if (ParseFlag(a, "threads", &v)) flags.threads = ParseInt("threads", v);
    else if (ParseFlag(a, "read_ratio", &v)) flags.read_ratio = ParseDouble("read_ratio", v);
    else if (ParseFlag(a, "update_ratio", &v)) flags.update_ratio = ParseDouble("update_ratio", v);
    else if (ParseFlag(a, "zipf", &v)) flags.zipf = ParseDouble("zipf", v);
    else if (ParseFlag(a, "key_dist", &v)) flags.key_dist = v;
    else if (ParseFlag(a, "hotspot", &v)) flags.hotspot = ParseDouble("hotspot", v);
    else if (ParseFlag(a, "life_range_size", &v)) flags.life_range_size = ParseU64("life_range_size", v);
    else if (ParseFlag(a, "life_hot_slots", &v)) flags.life_hot_slots = ParseInt("life_hot_slots", v);
    else if (ParseFlag(a, "life_lifetime_s", &v)) flags.life_lifetime_s = ParseDouble("life_lifetime_s", v);
    else if (ParseFlag(a, "life_ramp_frac", &v)) flags.life_ramp_frac = ParseDouble("life_ramp_frac", v);
    else if (ParseFlag(a, "life_cold_frac", &v)) flags.life_cold_frac = ParseDouble("life_cold_frac", v);
    else if (ParseFlag(a, "life_chain", &v)) flags.life_chain = ParseInt("life_chain", v);
    else if (ParseFlag(a, "life_chain_lag", &v)) flags.life_chain_lag = ParseDouble("life_chain_lag", v);
    else if (ParseFlag(a, "op_rate", &v)) flags.op_rate = ParseDouble("op_rate", v);
    else if (ParseFlag(a, "write_rate", &v)) flags.write_rate = ParseDouble("write_rate", v);
    else if (ParseFlag(a, "duration", &v)) flags.duration = ParseInt("duration", v);
    else if (ParseFlag(a, "warmup", &v)) flags.warmup = ParseInt("warmup", v);
    else if (ParseFlag(a, "fill", &v)) flags.fill = ParseBool("fill", v);
    else if (ParseFlag(a, "seed", &v)) flags.seed = ParseU64("seed", v);
    else if (ParseFlag(a, "trace_out", &v)) flags.trace_out = v;
    else if (ParseFlag(a, "policy", &v)) flags.policy = v;
    else if (ParseFlag(a, "model_prefix", &v)) flags.model_prefix = v;
    else if (ParseFlag(a, "model_steps", &v)) flags.model_steps = ParseInt("model_steps", v);
    else if (ParseFlag(a, "precursors", &v)) flags.precursors = v;
    else if (ParseFlag(a, "leaper_range_size", &v)) flags.leaper_range_size = ParseU64("leaper_range_size", v);
    else if (ParseFlag(a, "leaper_slot_s", &v)) flags.leaper_slot_s = ParseDouble("leaper_slot_s", v);
    else if (ParseFlag(a, "leaper_t1_alpha", &v)) flags.leaper_t1_alpha = ParseDouble("leaper_t1_alpha", v);
    else if (ParseFlag(a, "leaper_t2_beta", &v)) flags.leaper_t2_beta = ParseDouble("leaper_t2_beta", v);
    else if (ParseFlag(a, "leaper_threshold", &v)) flags.leaper_threshold = ParseDouble("leaper_threshold", v);
    else if (ParseFlag(a, "warm_scan_keys", &v)) flags.warm_scan_keys = ParseInt("warm_scan_keys", v);
    else { std::fprintf(stderr, "unknown flag: %s\n", a); std::exit(2); }
  }
}

KeyDist ParseKeyDist(const std::string& s) {
  if (s == "uniform") return KeyDist::kUniform;
  if (s == "scrambled") return KeyDist::kScrambled;
  if (s == "lifecycle") return KeyDist::kLifecycle;
  return KeyDist::kZipfContiguous;
}

std::string RandomValuePool(size_t bytes, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::string pool(bytes, '\0');
  for (size_t i = 0; i < bytes; i += 8) {
    const uint64_t r = rng();
    std::memcpy(&pool[i], &r, std::min<size_t>(8, bytes - i));
  }
  return pool;
}

struct Shared {
  rocksdb::DB* db = nullptr;
  leaper_rocksdb::Adapter* adapter = nullptr;
  const std::string* value_pool = nullptr;
  std::atomic<bool> running{false}, measuring{false};
  std::atomic<uint64_t> reads{0}, writes{0}, ops_issued{0}, writes_issued{0};
  std::atomic<uint64_t> next_insert_key{0};
  uint64_t run_start_us = 0, measure_start_us = 0;
  std::vector<Histogram*> read_hist, write_hist;
  std::vector<TraceWriter*> trace;
};

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

void WorkerLoop(Shared* s, int tid) {
  std::mt19937_64 rng(flags.seed * 1000003ULL + tid);
  Dynamics dyn;
  Lifecycle life;
  life.range_size = flags.life_range_size;
  life.hot_slots = flags.life_hot_slots;
  life.lifetime_s = flags.life_lifetime_s;
  life.ramp_frac = flags.life_ramp_frac;
  life.cold_frac = flags.life_cold_frac;
  life.chain = flags.life_chain;
  life.chain_lag = flags.life_chain_lag;
  life.seed = flags.seed;
  KeyChooser chooser(flags.num, ParseKeyDist(flags.key_dist), flags.zipf,
                     flags.hotspot, dyn, life);
  Histogram* rh = s->read_hist[tid];
  Histogram* wh = s->write_hist[tid];
  TraceWriter* tw = s->trace.empty() ? nullptr : s->trace[tid];
  rocksdb::ReadOptions ro;
  rocksdb::WriteOptions wo;
  std::string value;
  std::uniform_real_distribution<double> pick(0.0, 1.0);
  std::uniform_int_distribution<size_t> voff(
      0, s->value_pool->size() - flags.value_size - 1);

  while (s->running.load(std::memory_order_relaxed)) {
    uint64_t t0 = NowUs();
    if (flags.op_rate > 0.0) {
      const double elapsed = (t0 - s->run_start_us) / 1e6;
      const uint64_t budget = static_cast<uint64_t>(flags.op_rate * elapsed) + 1;
      if (s->ops_issued.load(std::memory_order_relaxed) >= budget) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        continue;
      }
    }
    s->ops_issued.fetch_add(1, std::memory_order_relaxed);

    double p = pick(rng);
    if (flags.write_rate > 0.0 && p >= flags.read_ratio) {
      const double elapsed = (t0 - s->run_start_us) / 1e6;
      const uint64_t budget = static_cast<uint64_t>(flags.write_rate * elapsed) + 1;
      if (s->writes_issued.load(std::memory_order_relaxed) >= budget) p = 0.0;
    }

    const bool measuring = s->measuring.load(std::memory_order_relaxed);
    const double elapsed_s = (t0 - s->run_start_us) / 1e6;
    uint64_t idx;
    OpType op;
    if (p < flags.read_ratio) {
      idx = chooser.Next(&rng, elapsed_s);
      op = kOpRead;
      const std::string key = EncodeKey(idx);
      if (s->adapter != nullptr) s->adapter->OnRead(key);
      t0 = NowUs();
      s->db->Get(ro, key, &value);
      const uint64_t dt = NowUs() - t0;
      if (measuring) {
        rh->Add(dt);
        s->reads.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      if (p < flags.read_ratio + flags.update_ratio) {
        idx = chooser.Next(&rng, elapsed_s);
        op = kOpUpdate;
      } else {
        idx = s->next_insert_key.fetch_add(1, std::memory_order_relaxed);
        op = kOpInsert;
      }
      const std::string key = EncodeKey(idx);
      if (s->adapter != nullptr) s->adapter->OnWrite(key);
      s->writes_issued.fetch_add(1, std::memory_order_relaxed);
      t0 = NowUs();
      s->db->Put(wo, key, rocksdb::Slice(s->value_pool->data() + voff(rng),
                                         flags.value_size));
      const uint64_t dt = NowUs() - t0;
      if (measuring) {
        wh->Add(dt);
        s->writes.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (tw != nullptr && measuring) {
      tw->Add(static_cast<uint32_t>((t0 - s->measure_start_us) / 1000), idx, op);
    }
  }
}

int Run() {
  std::fprintf(stderr,
      "[config] rocksdb db=%s num=%" PRIu64 " cache_mb=%d threads=%d "
      "policy=%s key_dist=%s duration=%d warmup=%d\n",
      flags.db.c_str(), flags.num, flags.cache_mb, flags.threads,
      flags.policy.c_str(), flags.key_dist.c_str(), flags.duration, flags.warmup);

  std::unique_ptr<leaper_rocksdb::Adapter> adapter;
  if (flags.policy == "leaper") {
    leaper_rocksdb::AdapterOptions ao;
    ao.core.policy = leaper::Policy::kLeaper;
    ao.core.slot_seconds = flags.leaper_slot_s;
    ao.core.range_size = flags.leaper_range_size;
    ao.core.hot_threshold = flags.leaper_threshold;
    ao.core.t1_alpha = flags.leaper_t1_alpha;
    ao.core.t2_beta = flags.leaper_t2_beta;
    ao.core.cache_bytes = static_cast<double>(flags.cache_mb) * 1024 * 1024;
    ao.core.precursor_path = flags.precursors;
    ao.num_ranges = flags.num / flags.leaper_range_size + 1;
    ao.warm_scan_keys = flags.warm_scan_keys;
    for (int k = 1; k <= flags.model_steps; ++k) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s.step%d.txt", flags.model_prefix.c_str(), k);
      ao.core.model_paths.push_back(path);
    }
    std::string err;
    adapter = leaper_rocksdb::Adapter::Create(ao, &err);
    if (adapter == nullptr) {
      std::fprintf(stderr, "leaper init failed: %s\n", err.c_str());
      return 2;
    }
  }

  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = static_cast<size_t>(flags.write_buffer_mb) * 1024 * 1024;
  opts.target_file_size_base = static_cast<uint64_t>(flags.max_file_mb) * 1024 * 1024;
  opts.compression = rocksdb::kNoCompression;
  opts.statistics = rocksdb::CreateDBStatistics();
  opts.max_background_jobs = 2;

  rocksdb::BlockBasedTableOptions bbt;
  bbt.block_cache = rocksdb::NewLRUCache(
      static_cast<size_t>(flags.cache_mb) * 1024 * 1024);
  bbt.block_size = static_cast<size_t>(flags.block_kb) * 1024;
  bbt.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
  bbt.cache_index_and_filter_blocks = false;
  if (flags.policy == "flush_only") {
    bbt.prepopulate_block_cache =
        rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kFlushOnly;
  } else if (flags.policy == "flush_and_compaction") {
    bbt.prepopulate_block_cache =
        rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kFlushAndCompaction;
  }
  opts.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbt));
  if (adapter != nullptr) opts.listeners.push_back(adapter->listener());

  if (flags.fill) rocksdb::DestroyDB(flags.db, rocksdb::Options());
  // RocksDB 11 changed DB::Open to hand back a unique_ptr.
  std::unique_ptr<rocksdb::DB> db_owner;
  rocksdb::Status st = rocksdb::DB::Open(opts, flags.db, &db_owner);
  if (!st.ok()) {
    std::fprintf(stderr, "open failed: %s\n", st.ToString().c_str());
    return 1;
  }
  rocksdb::DB* db = db_owner.get();
  if (adapter != nullptr) adapter->SetDB(db);

  const std::string pool = RandomValuePool(1 << 20, flags.seed);
  if (flags.fill) {
    std::fprintf(stderr, "[fill] writing %" PRIu64 " records...\n", flags.num);
    std::mt19937_64 rng(flags.seed);
    std::uniform_int_distribution<size_t> voff(0, pool.size() - flags.value_size - 1);
    rocksdb::WriteBatch batch;
    const uint64_t t0 = NowUs();
    for (uint64_t i = 0; i < flags.num; ++i) {
      batch.Put(EncodeKey(i),
                rocksdb::Slice(pool.data() + voff(rng), flags.value_size));
      if ((i + 1) % 1000 == 0) {
        db->Write(rocksdb::WriteOptions(), &batch);
        batch.Clear();
      }
    }
    db->Write(rocksdb::WriteOptions(), &batch);
    std::fprintf(stderr, "[fill] done in %.1fs; compacting\n", (NowUs() - t0) / 1e6);
    db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
  }

  Shared shared;
  shared.db = db;
  shared.adapter = adapter.get();
  shared.value_pool = &pool;
  shared.next_insert_key.store(flags.num);
  for (int i = 0; i < flags.threads; ++i) {
    shared.read_hist.push_back(new Histogram());
    shared.write_hist.push_back(new Histogram());
  }
  if (!flags.trace_out.empty()) {
    for (int i = 0; i < flags.threads; ++i) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s.trace.%d", flags.trace_out.c_str(), i);
      shared.trace.push_back(new TraceWriter(path));
    }
    std::FILE* meta = std::fopen((flags.trace_out + ".meta").c_str(), "w");
    std::fprintf(meta, "engine=rocksdb\nnum_keys=%" PRIu64 "\nlife_range_size=%" PRIu64
                 "\nlife_chain=%d\nseed=%" PRIu64 "\nthreads=%d\n",
                 flags.num, flags.life_range_size, flags.life_chain, flags.seed,
                 flags.threads);
    std::fclose(meta);
  }

  std::FILE* csv = std::fopen((flags.out_prefix + ".timeseries.csv").c_str(), "w");
  std::fprintf(csv, "t,qps,read_qps,write_qps,block_lookups,block_hits,hit_ratio,"
                    "read_p50_us,read_p95_us,read_p99_us,write_p99_us,"
                    "compactions_running,compactions_done,flushes_done,trivial_moves,"
                    "cache_live_mb,cache_stale_mb,stale_ids,sst_files,cache_charge_mb\n");

  shared.run_start_us = NowUs();
  shared.measure_start_us = shared.run_start_us +
      static_cast<uint64_t>(flags.warmup) * 1000000ULL;
  if (adapter != nullptr) adapter->ResetClock();
  shared.running.store(true);
  std::vector<std::thread> workers;
  for (int i = 0; i < flags.threads; ++i) workers.emplace_back(WorkerLoop, &shared, i);

  const int hist_n = Histogram::kNumBuckets;
  std::vector<uint64_t> rprev(hist_n, 0), wprev(hist_n, 0);
  uint64_t prev_reads = 0, prev_writes = 0, prev_hit = 0, prev_miss = 0;
  uint64_t prev_flush = 0, prev_comp = 0;

  auto ticker = [&](uint32_t t) { return opts.statistics->getTickerCount(t); };

  for (int sec = 1; sec <= flags.warmup + flags.duration; ++sec) {
    if (sec == flags.warmup + 1) {
      shared.measuring.store(true);
      rprev.assign(hist_n, 0);
      wprev.assign(hist_n, 0);
      for (auto* h : shared.read_hist) h->AccumulateInto(&rprev);
      for (auto* h : shared.write_hist) h->AccumulateInto(&wprev);
      prev_reads = shared.reads.load();
      prev_writes = shared.writes.load();
      prev_hit = ticker(rocksdb::BLOCK_CACHE_DATA_HIT);
      prev_miss = ticker(rocksdb::BLOCK_CACHE_DATA_MISS);
      prev_flush = ticker(rocksdb::FLUSH_WRITE_BYTES);
      prev_comp = ticker(rocksdb::COMPACT_WRITE_BYTES);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (sec <= flags.warmup) continue;

    const uint64_t reads = shared.reads.load(), writes = shared.writes.load();
    const uint64_t hit = ticker(rocksdb::BLOCK_CACHE_DATA_HIT);
    const uint64_t miss = ticker(rocksdb::BLOCK_CACHE_DATA_MISS);
    const uint64_t flush_b = ticker(rocksdb::FLUSH_WRITE_BYTES);
    const uint64_t comp_b = ticker(rocksdb::COMPACT_WRITE_BYTES);
    std::vector<uint64_t> rcur(hist_n, 0), wcur(hist_n, 0);
    for (auto* h : shared.read_hist) h->AccumulateInto(&rcur);
    for (auto* h : shared.write_hist) h->AccumulateInto(&wcur);

    const uint64_t dh = hit - prev_hit, dm = miss - prev_miss;
    const uint64_t look = dh + dm;
    if (adapter != nullptr) {
      adapter->set_qps(static_cast<double>(reads + writes) /
                       std::max(1, sec - flags.warmup));
    }
    const int t = sec - flags.warmup;
    std::fprintf(csv,
        "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.5f,"
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",0,%" PRIu64 ",%" PRIu64
        ",0,0,0,0,0,0\n",
        t, (reads - prev_reads) + (writes - prev_writes), reads - prev_reads,
        writes - prev_writes, look, dh,
        look ? static_cast<double>(dh) / look : 0.0,
        Percentile(rcur, rprev, 0.50), Percentile(rcur, rprev, 0.95),
        Percentile(rcur, rprev, 0.99), Percentile(wcur, wprev, 0.99),
        (comp_b - prev_comp) / (1u << 20), (flush_b - prev_flush) / (1u << 20));
    std::fflush(csv);
    std::fprintf(stderr, "t=%3d qps=%7" PRIu64 " hit=%.4f p95=%5" PRIu64 "us\n",
                 t, (reads - prev_reads) + (writes - prev_writes),
                 look ? static_cast<double>(dh) / look : 0.0,
                 Percentile(rcur, rprev, 0.95));

    rprev.swap(rcur); wprev.swap(wcur);
    prev_reads = reads; prev_writes = writes;
    prev_hit = hit; prev_miss = miss;
    prev_flush = flush_b; prev_comp = comp_b;
  }

  shared.running.store(false);
  for (auto& t : workers) t.join();
  for (auto* tw : shared.trace) { tw->Close(); delete tw; }
  std::fclose(csv);

  const uint64_t hit = ticker(rocksdb::BLOCK_CACHE_DATA_HIT);
  const uint64_t miss = ticker(rocksdb::BLOCK_CACHE_DATA_MISS);
  std::fprintf(stderr, "\n[summary] block cache: %" PRIu64 " hits, %" PRIu64
               " misses (%.4f hit ratio)\n",
               hit, miss, (hit + miss) ? static_cast<double>(hit) / (hit + miss) : 0.0);
  if (adapter != nullptr) {
    const leaper::Stats ls = adapter->stats();
    std::fprintf(stderr,
        "[leaper] reads_seen=%" PRIu64 " inferences=%" PRIu64 " (%.2f us/inf) "
        "hot=%" PRIu64 " warmed_ranges=%" PRIu64 " warm_us=%" PRIu64 "\n",
        ls.reads_seen, ls.inferences,
        ls.inferences ? static_cast<double>(ls.inference_us) / ls.inferences : 0.0,
        ls.ranges_predicted_hot, adapter->warmed_ranges(), adapter->warm_us());
  }
  for (auto* h : shared.read_hist) delete h;
  for (auto* h : shared.write_hist) delete h;
  db_owner.reset();
  return 0;
}

}  // namespace
}  // namespace leaper_bench

int main(int argc, char** argv) {
  leaper_bench::ParseArgs(argc, argv);
  return leaper_bench::Run();
}
