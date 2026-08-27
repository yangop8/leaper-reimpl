// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// leaper_bench -- M0 harness for the Leaper reproduction.
//
// Goal of M0: establish, on *stock* LevelDB with no core patches, that
// background operations invalidate the block cache and that the invalidation
// is visible as a drop in block cache hit ratio, a drop in QPS and a spike in
// tail latency (the phenomenon of Figure 1 in the VLDB'20 paper).
//
// Everything here is an observer: a Cache decorator (StatsCache), an
// Options::info_log decorator (EventLogger) and a workload driver. No LevelDB
// source file is modified, so these numbers are a valid baseline.

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"

#include "leaper_bench/event_logger.h"
#include "leaper_bench/histogram.h"
#include "leaper_bench/keygen.h"
#include "leaper_bench/pread_env.h"
#include "leaper_bench/stats_cache.h"
#include "leaper_bench/trace.h"

#include "leaper_leveldb.h"

namespace leaper_bench {
namespace {

struct Flags {
  std::string db = "/tmp/leaper_m0_db";
  std::string out_prefix = "experiments/results/m0";
  uint64_t num = 8000000;
  int value_size = 100;
  int cache_mb = 96;
  int write_buffer_mb = 16;
  int max_file_mb = 8;
  int block_kb = 4;
  int max_open_files = 1000;
  int bloom_bits = 10;
  int threads = 8;
  double read_ratio = 0.75;
  double update_ratio = 0.20;   // remainder is inserts
  double zipf = 0.99;
  double write_rate = 0.0;      // target writes/sec across all threads; 0 = closed loop
  double op_rate = 0.0;         // target total ops/sec across all threads; 0 = unthrottled
  int read_delay_us = 0;        // emulated device latency per block read
  std::string trace_out;        // prefix for the M1 access trace; empty = off
  double trace_sample = 1.0;
  std::string key_dist = "zipf";
  double hotspot = 0.0;
  double hotspot_shift = 0.0;   // keys/s the hot region advances (drift)
  int phases = 0;               // hot-region rotation, 0/1 = off
  double phase_period_s = 0.0;
  uint64_t life_range_size = 10000;
  int life_hot_slots = 64;
  double life_lifetime_s = 60.0;
  double life_ramp_frac = 0.25;
  double life_cold_frac = 0.0;
  int life_chain = 1;
  double life_chain_lag = 0.2;
  // Correlation between the read hot set and the write hot set.
  //
  // At 1.0 (the original behaviour) reads and writes hit the same ranges, so
  // every block a compaction writes is by construction read-hot and "warm
  // everything" is handed a perfect prediction for free. Real workloads are
  // rarely like that -- a catalogue is read while an order log is written --
  // and the value of *choosing* what to warm lives entirely in the gap. This
  // draws each write from the read hot set with probability write_corr and
  // from an independent hot set otherwise.
  double write_corr = 1.0;
  int duration = 180;
  int warmup = 20;
  int stale_after = 10;
  bool track_cache_ids = true;
  bool mmap_reads = false;      // see pread_env.h -- mmap bypasses the block cache
  bool bypass_page_cache = true;
  bool fill = true;
  bool compact_after_fill = true;
  std::string compression = "none";
  uint64_t seed = 42;

  // --- Leaper plug-in ---
  std::string policy = "off";
  std::string model_prefix;      // expects <prefix>.step1.txt .. .stepK.txt
  int model_steps = 1;
  std::string precursors;
  std::string oracle;
  uint64_t leaper_range_size = 40000;
  double leaper_slot_s = 1.0;
  int leaper_history = 6;
  double leaper_sample = 1.0;
  double leaper_threshold = 0.5;
  double leaper_t1_alpha = 2.77e-5;
  double leaper_t2_beta = 3709.0;
  double leaper_t1_seconds = 0.0;
  double leaper_t2_seconds = 0.0;
  double leaper_max_prefetch_frac = 1.0;  // no cap by default: WarmAll must mean warm all
  std::string key_format = "decimal";
  bool leaper_phase1 = true;
  bool leaper_phase2 = true;
};

Flags flags;

bool ParseFlag(const char* arg, const char* name, std::string* out) {
  const size_t n = std::strlen(name);
  if (std::strncmp(arg, "--", 2) != 0) return false;
  if (std::strncmp(arg + 2, name, n) != 0 || arg[2 + n] != '=') return false;
  *out = arg + 3 + n;
  return true;
}

// Numeric flags are validated to consume their whole value. Without this a
// shell mistake such as passing a whole flag string as one argument (zsh does
// not word-split unquoted parameter expansions) silently yields a run with
// default settings for every flag but the first -- an experiment that looks
// fine and measures the wrong thing.
[[noreturn]] void BadValue(const char* name, const std::string& v) {
  std::fprintf(stderr, "invalid value for --%s: '%s'\n", name, v.c_str());
  std::exit(2);
}

uint64_t ParseU64(const char* name, const std::string& v) {
  char* end = nullptr;
  const uint64_t r = std::strtoull(v.c_str(), &end, 10);
  if (v.empty() || end == nullptr || *end != '\0') BadValue(name, v);
  return r;
}

int ParseInt(const char* name, const std::string& v) {
  char* end = nullptr;
  const long r = std::strtol(v.c_str(), &end, 10);
  if (v.empty() || end == nullptr || *end != '\0') BadValue(name, v);
  return static_cast<int>(r);
}

double ParseDouble(const char* name, const std::string& v) {
  char* end = nullptr;
  const double r = std::strtod(v.c_str(), &end);
  if (v.empty() || end == nullptr || *end != '\0') BadValue(name, v);
  return r;
}

bool ParseBool(const char* name, const std::string& v) {
  if (v == "1" || v == "true") return true;
  if (v == "0" || v == "false") return false;
  BadValue(name, v);
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
    else if (ParseFlag(a, "max_open_files", &v)) flags.max_open_files = ParseInt("max_open_files", v);
    else if (ParseFlag(a, "bloom_bits", &v)) flags.bloom_bits = ParseInt("bloom_bits", v);
    else if (ParseFlag(a, "threads", &v)) flags.threads = ParseInt("threads", v);
    else if (ParseFlag(a, "read_ratio", &v)) flags.read_ratio = ParseDouble("read_ratio", v);
    else if (ParseFlag(a, "update_ratio", &v)) flags.update_ratio = ParseDouble("update_ratio", v);
    else if (ParseFlag(a, "zipf", &v)) flags.zipf = ParseDouble("zipf", v);
    else if (ParseFlag(a, "write_rate", &v)) flags.write_rate = ParseDouble("write_rate", v);
    else if (ParseFlag(a, "op_rate", &v)) flags.op_rate = ParseDouble("op_rate", v);
    else if (ParseFlag(a, "read_delay_us", &v)) flags.read_delay_us = ParseInt("read_delay_us", v);
    else if (ParseFlag(a, "trace_out", &v)) flags.trace_out = v;
    else if (ParseFlag(a, "trace_sample", &v)) flags.trace_sample = ParseDouble("trace_sample", v);
    else if (ParseFlag(a, "key_dist", &v)) flags.key_dist = v;
    else if (ParseFlag(a, "hotspot", &v)) flags.hotspot = ParseDouble("hotspot", v);
    else if (ParseFlag(a, "hotspot_shift", &v)) flags.hotspot_shift = ParseDouble("hotspot_shift", v);
    else if (ParseFlag(a, "phases", &v)) flags.phases = ParseInt("phases", v);
    else if (ParseFlag(a, "phase_period_s", &v)) flags.phase_period_s = ParseDouble("phase_period_s", v);
    else if (ParseFlag(a, "life_range_size", &v)) flags.life_range_size = ParseU64("life_range_size", v);
    else if (ParseFlag(a, "life_hot_slots", &v)) flags.life_hot_slots = ParseInt("life_hot_slots", v);
    else if (ParseFlag(a, "life_lifetime_s", &v)) flags.life_lifetime_s = ParseDouble("life_lifetime_s", v);
    else if (ParseFlag(a, "life_ramp_frac", &v)) flags.life_ramp_frac = ParseDouble("life_ramp_frac", v);
    else if (ParseFlag(a, "life_cold_frac", &v)) flags.life_cold_frac = ParseDouble("life_cold_frac", v);
    else if (ParseFlag(a, "life_chain", &v)) flags.life_chain = ParseInt("life_chain", v);
    else if (ParseFlag(a, "life_chain_lag", &v)) flags.life_chain_lag = ParseDouble("life_chain_lag", v);
    else if (ParseFlag(a, "write_corr", &v)) flags.write_corr = ParseDouble("write_corr", v);
    else if (ParseFlag(a, "duration", &v)) flags.duration = ParseInt("duration", v);
    else if (ParseFlag(a, "warmup", &v)) flags.warmup = ParseInt("warmup", v);
    else if (ParseFlag(a, "stale_after", &v)) flags.stale_after = ParseInt("stale_after", v);
    else if (ParseFlag(a, "track_cache_ids", &v)) flags.track_cache_ids = ParseBool("track_cache_ids", v);
    else if (ParseFlag(a, "mmap_reads", &v)) flags.mmap_reads = ParseBool("mmap_reads", v);
    else if (ParseFlag(a, "bypass_page_cache", &v)) flags.bypass_page_cache = ParseBool("bypass_page_cache", v);
    else if (ParseFlag(a, "fill", &v)) flags.fill = ParseBool("fill", v);
    else if (ParseFlag(a, "compact_after_fill", &v)) flags.compact_after_fill = ParseBool("compact_after_fill", v);
    else if (ParseFlag(a, "compression", &v)) flags.compression = v;
    else if (ParseFlag(a, "seed", &v)) flags.seed = ParseU64("seed", v);
    else if (ParseFlag(a, "policy", &v)) flags.policy = v;
    else if (ParseFlag(a, "model_prefix", &v)) flags.model_prefix = v;
    else if (ParseFlag(a, "model_steps", &v)) flags.model_steps = ParseInt("model_steps", v);
    else if (ParseFlag(a, "precursors", &v)) flags.precursors = v;
    else if (ParseFlag(a, "oracle", &v)) flags.oracle = v;
    else if (ParseFlag(a, "leaper_range_size", &v)) flags.leaper_range_size = ParseU64("leaper_range_size", v);
    else if (ParseFlag(a, "leaper_slot_s", &v)) flags.leaper_slot_s = ParseDouble("leaper_slot_s", v);
    else if (ParseFlag(a, "leaper_history", &v)) flags.leaper_history = ParseInt("leaper_history", v);
    else if (ParseFlag(a, "leaper_sample", &v)) flags.leaper_sample = ParseDouble("leaper_sample", v);
    else if (ParseFlag(a, "leaper_threshold", &v)) flags.leaper_threshold = ParseDouble("leaper_threshold", v);
    else if (ParseFlag(a, "leaper_t1_alpha", &v)) flags.leaper_t1_alpha = ParseDouble("leaper_t1_alpha", v);
    else if (ParseFlag(a, "leaper_t2_beta", &v)) flags.leaper_t2_beta = ParseDouble("leaper_t2_beta", v);
    else if (ParseFlag(a, "leaper_t1_seconds", &v)) flags.leaper_t1_seconds = ParseDouble("leaper_t1_seconds", v);
    else if (ParseFlag(a, "leaper_t2_seconds", &v)) flags.leaper_t2_seconds = ParseDouble("leaper_t2_seconds", v);
    else if (ParseFlag(a, "leaper_max_prefetch_frac", &v)) flags.leaper_max_prefetch_frac = ParseDouble("leaper_max_prefetch_frac", v);
    else if (ParseFlag(a, "key_format", &v)) flags.key_format = v;
    else if (ParseFlag(a, "leaper_phase1", &v)) flags.leaper_phase1 = ParseBool("leaper_phase1", v);
    else if (ParseFlag(a, "leaper_phase2", &v)) flags.leaper_phase2 = ParseBool("leaper_phase2", v);
    else { std::fprintf(stderr, "unknown flag: %s\n", a); std::exit(2); }
  }
}

KeyDist ParseKeyDist(const std::string& s) {
  if (s == "uniform") return KeyDist::kUniform;
  if (s == "scrambled") return KeyDist::kScrambled;
  if (s == "lifecycle") return KeyDist::kLifecycle;
  return KeyDist::kZipfContiguous;
}

// ---------------------------------------------------------------------------

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
  leveldb::DB* db = nullptr;
  StatsCache* cache = nullptr;
  EventLogger* logger = nullptr;
  const std::string* value_pool = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> measuring{false};
  std::atomic<uint64_t> reads{0}, writes{0}, read_hits{0}, read_misses{0};
  std::atomic<uint64_t> next_insert_key{0};
  std::atomic<uint64_t> writes_issued{0};
  std::atomic<uint64_t> ops_issued{0};
  uint64_t run_start_us = 0;
  uint64_t measure_start_us = 0;
  std::vector<TraceWriter*> trace;
  std::vector<Histogram*> read_hist, write_hist;
};

void WorkerLoop(Shared* s, int tid) {
  std::mt19937_64 rng(flags.seed * 1000003ULL + tid);
  Dynamics dyn;
  dyn.shift_per_s = flags.hotspot_shift;
  dyn.phases = flags.phases;
  dyn.phase_period_s = flags.phase_period_s;
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
  // Independent hot set for the decorrelated share of writes.
  Lifecycle wlife = life;
  wlife.seed = life.seed ^ 0x5DEECE66DULL;
  KeyChooser write_chooser(flags.num, ParseKeyDist(flags.key_dist), flags.zipf,
                           flags.hotspot + 0.5, dyn, wlife);
  Histogram* rh = s->read_hist[tid];
  Histogram* wh = s->write_hist[tid];
  TraceWriter* tw = s->trace.empty() ? nullptr : s->trace[tid];
  leveldb::ReadOptions ropts;
  leveldb::WriteOptions wopts;  // sync = false
  std::string value;
  std::uniform_real_distribution<double> pick(0.0, 1.0);
  std::uniform_int_distribution<size_t> voff(
      0, s->value_pool->size() - flags.value_size - 1);
  leveldb::Env* env = leveldb::Env::Default();

  while (s->running.load(std::memory_order_relaxed)) {
    uint64_t t0 = env->NowMicros();

    if (flags.op_rate > 0.0) {
      const double elapsed = (t0 - s->run_start_us) / 1e6;
      const uint64_t budget = static_cast<uint64_t>(flags.op_rate * elapsed) + 1;
      if (s->ops_issued.load(std::memory_order_relaxed) >= budget) {
        env->SleepForMicroseconds(200);
        continue;
      }
    }
    s->ops_issued.fetch_add(1, std::memory_order_relaxed);

    double p = pick(rng);
    // A closed-loop driver makes write pressure track read speed, so the DB
    // sits in permanent compaction and there is no quiet period to contrast
    // against. Pace writes against wall clock instead and spend the surplus on
    // reads; --write_rate then sets the compaction cadence directly.
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
      t0 = env->NowMicros();
      const leveldb::Status st = s->db->Get(ropts, key, &value);
      const uint64_t dt = env->NowMicros() - t0;
      if (measuring) {
        rh->Add(dt);
        s->reads.fetch_add(1, std::memory_order_relaxed);
        if (st.ok()) s->read_hits.fetch_add(1, std::memory_order_relaxed);
        else s->read_misses.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      if (p < flags.read_ratio + flags.update_ratio) {
        idx = (flags.write_corr >= 1.0 || pick(rng) < flags.write_corr)
                  ? chooser.Next(&rng, elapsed_s)
                  : write_chooser.Next(&rng, elapsed_s);
        op = kOpUpdate;
      } else {
        idx = s->next_insert_key.fetch_add(1, std::memory_order_relaxed);
        op = kOpInsert;
      }
      const std::string key = EncodeKey(idx);
      const leveldb::Slice val(s->value_pool->data() + voff(rng), flags.value_size);
      s->writes_issued.fetch_add(1, std::memory_order_relaxed);
      t0 = env->NowMicros();
      s->db->Put(wopts, key, val);
      const uint64_t dt = env->NowMicros() - t0;
      if (measuring) {
        wh->Add(dt);
        s->writes.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (tw != nullptr && measuring &&
        (flags.trace_sample >= 1.0 || pick(rng) < flags.trace_sample)) {
      tw->Add(static_cast<uint32_t>((t0 - s->measure_start_us) / 1000), idx, op);
    }
  }
}

uint64_t TotalFiles(leveldb::DB* db) {
  uint64_t total = 0;
  for (int level = 0; level < 7; ++level) {
    std::string prop;
    char name[64];
    std::snprintf(name, sizeof(name), "leveldb.num-files-at-level%d", level);
    if (db->GetProperty(name, &prop)) total += std::strtoull(prop.c_str(), nullptr, 10);
  }
  return total;
}

int Run() {
  std::fprintf(stderr,
      "[config] db=%s num=%" PRIu64 " value_size=%d cache_mb=%d write_buffer_mb=%d\n"
      "[config] max_file_mb=%d block_kb=%d threads=%d read_ratio=%.2f "
      "update_ratio=%.2f zipf=%.2f key_dist=%s\n"
      "[config] write_rate=%.0f op_rate=%.0f read_delay_us=%d mmap_reads=%d "
      "bypass_page_cache=%d fill=%d duration=%d warmup=%d\n",
      flags.db.c_str(), flags.num, flags.value_size, flags.cache_mb,
      flags.write_buffer_mb, flags.max_file_mb, flags.block_kb, flags.threads,
      flags.read_ratio, flags.update_ratio, flags.zipf, flags.key_dist.c_str(),
      flags.write_rate, flags.op_rate, flags.read_delay_us, flags.mmap_reads ? 1 : 0,
      flags.bypass_page_cache ? 1 : 0, flags.fill ? 1 : 0, flags.duration, flags.warmup);
  leveldb::Env* env = leveldb::Env::Default();
  const uint64_t start_us = env->NowMicros();

  // An aborted run leaves its .events.csv behind (it is only written at the
  // end), and the next analysis silently pairs a fresh timeseries with a stale
  // event timeline. Remove the outputs up front so a crash cannot fake a
  // complete result set.
  std::remove((flags.out_prefix + ".events.csv").c_str());
  std::remove((flags.out_prefix + ".timeseries.csv").c_str());

  const std::string log_path = flags.out_prefix + ".leveldb.log";
  std::FILE* log_sink = std::fopen(log_path.c_str(), "w");
  EventLogger* logger = new EventLogger(start_us, log_sink);
  StatsCache* cache = new StatsCache(
      leveldb::NewLRUCache(static_cast<size_t>(flags.cache_mb) * 1024 * 1024),
      flags.track_cache_ids);

  PreadEnv* pread_env = flags.mmap_reads
      ? nullptr
      : new PreadEnv(leveldb::Env::Default(), flags.bypass_page_cache,
                     flags.read_delay_us);

  // --- Leaper plug-in -------------------------------------------------
  std::unique_ptr<leaper_leveldb::Adapter> adapter;
  if (flags.policy != "off") {
    leaper_leveldb::AdapterOptions ao;
    ao.key_format = flags.key_format;
    if (!leaper::ParsePolicy(flags.policy, &ao.core.policy)) {
      std::fprintf(stderr, "unknown --policy: %s\n", flags.policy.c_str());
      return 2;
    }
    ao.core.slot_seconds = flags.leaper_slot_s;
    ao.core.history_slots = flags.leaper_history;
    ao.core.sample_rate = flags.leaper_sample;
    ao.core.range_size = flags.leaper_range_size;
    ao.core.max_range_id = flags.num / flags.leaper_range_size + 1;
    ao.core.hot_threshold = flags.leaper_threshold;
    ao.core.t1_alpha = flags.leaper_t1_alpha;
    ao.core.t2_beta = flags.leaper_t2_beta;
    ao.core.t1_seconds = flags.leaper_t1_seconds;
    ao.core.t2_seconds = flags.leaper_t2_seconds;
    ao.core.cache_bytes = static_cast<double>(flags.cache_mb) * 1024 * 1024;
    ao.core.max_prefetch_frac = flags.leaper_max_prefetch_frac;
    ao.core.enable_phase1 = flags.leaper_phase1;
    ao.core.enable_phase2 = flags.leaper_phase2;
    ao.core.precursor_path = flags.precursors;
    ao.core.oracle_path = flags.oracle;
    for (int k = 1; k <= flags.model_steps && !flags.model_prefix.empty(); ++k) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s.step%d.txt", flags.model_prefix.c_str(), k);
      ao.core.model_paths.push_back(path);
    }
    std::string err;
    adapter = leaper_leveldb::Adapter::Create(ao, &err);
    if (adapter == nullptr) {
      std::fprintf(stderr, "leaper init failed: %s\n", err.c_str());
      return 2;
    }
    std::fprintf(stderr, "[leaper] policy=%s range_size=%" PRIu64 " slot=%.2fs "
                 "models=%zu sample=%.3f\n",
                 flags.policy.c_str(), flags.leaper_range_size, flags.leaper_slot_s,
                 ao.core.model_paths.size(), flags.leaper_sample);
  }

  leveldb::Options opts;
  opts.create_if_missing = true;
  opts.leaper_hooks = adapter.get();
  if (pread_env != nullptr) opts.env = pread_env;
  opts.block_cache = cache;
  opts.info_log = logger;
  opts.write_buffer_size = static_cast<size_t>(flags.write_buffer_mb) * 1024 * 1024;
  opts.max_file_size = static_cast<size_t>(flags.max_file_mb) * 1024 * 1024;
  opts.block_size = static_cast<size_t>(flags.block_kb) * 1024;
  opts.max_open_files = flags.max_open_files;
  opts.compression = (flags.compression == "snappy") ? leveldb::kSnappyCompression
                                                     : leveldb::kNoCompression;
  if (flags.bloom_bits > 0) opts.filter_policy = leveldb::NewBloomFilterPolicy(flags.bloom_bits);

  if (flags.fill) {
    leveldb::DestroyDB(flags.db, leveldb::Options());
  }

  leveldb::DB* db = nullptr;
  leveldb::Status st = leveldb::DB::Open(opts, flags.db, &db);
  if (!st.ok()) {
    std::fprintf(stderr, "open failed: %s\n", st.ToString().c_str());
    return 1;
  }

  const std::string pool = RandomValuePool(1 << 20, flags.seed);

  if (flags.fill) {
    std::fprintf(stderr, "[fill] writing %" PRIu64 " records...\n", flags.num);
    std::mt19937_64 rng(flags.seed);
    std::uniform_int_distribution<size_t> voff(0, pool.size() - flags.value_size - 1);
    leveldb::WriteOptions wopts;
    leveldb::WriteBatch batch;
    const uint64_t t0 = env->NowMicros();
    for (uint64_t i = 0; i < flags.num; ++i) {
      batch.Put(EncodeKey(i), leveldb::Slice(pool.data() + voff(rng), flags.value_size));
      if ((i + 1) % 1000 == 0) {
        db->Write(wopts, &batch);
        batch.Clear();
        if ((i + 1) % 1000000 == 0) {
          std::fprintf(stderr, "[fill] %" PRIu64 "M records, %.1fs\n",
                       (i + 1) / 1000000, (env->NowMicros() - t0) / 1e6);
        }
      }
    }
    db->Write(wopts, &batch);
    std::fprintf(stderr, "[fill] done in %.1fs\n", (env->NowMicros() - t0) / 1e6);
    if (flags.compact_after_fill) {
      std::fprintf(stderr, "[fill] full compaction to reach a stable leveled shape...\n");
      const uint64_t c0 = env->NowMicros();
      db->CompactRange(nullptr, nullptr);
      std::fprintf(stderr, "[fill] compacted in %.1fs\n", (env->NowMicros() - c0) / 1e6);
    }
  }

  Shared shared;
  shared.db = db;
  shared.cache = cache;
  shared.logger = logger;
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
      TraceWriter* tw = new TraceWriter(path);
      if (!tw->ok()) {
        std::fprintf(stderr, "cannot write trace %s\n", path);
        return 1;
      }
      shared.trace.push_back(tw);
    }
    const std::string meta_path = flags.trace_out + ".meta";
    std::FILE* meta = std::fopen(meta_path.c_str(), "w");
    std::fprintf(meta,
        "# leaper access trace; records are 8 bytes: uint32 t_ms, "
        "uint32 (key_index << 2 | op); op 0=read 1=update 2=insert\n"
        "threads=%d\nnum_keys=%" PRIu64 "\nvalue_size=%d\nzipf=%.4f\n"
        "key_dist=%s\nhotspot=%.4f\nhotspot_shift=%.4f\nphases=%d\n"
        "phase_period_s=%.4f\nlife_range_size=%" PRIu64 "\n"
        "life_hot_slots=%d\nlife_lifetime_s=%.4f\nlife_ramp_frac=%.4f\n"
        "life_cold_frac=%.4f\nlife_chain=%d\nlife_chain_lag=%.4f\n"
        "write_corr=%.4f\nread_ratio=%.4f\n"
        "update_ratio=%.4f\n"
        "write_rate=%.1f\nop_rate=%.1f\nduration_s=%d\ntrace_sample=%.4f\n"
        "cache_mb=%d\nwrite_buffer_mb=%d\nmax_file_mb=%d\nblock_kb=%d\n"
        "read_delay_us=%d\nseed=%" PRIu64 "\n",
        flags.threads, flags.num, flags.value_size, flags.zipf,
        flags.key_dist.c_str(), flags.hotspot, flags.hotspot_shift, flags.phases,
        flags.phase_period_s, flags.life_range_size, flags.life_hot_slots,
        flags.life_lifetime_s, flags.life_ramp_frac, flags.life_cold_frac,
        flags.life_chain, flags.life_chain_lag, flags.write_corr,
        flags.read_ratio, flags.update_ratio,
        flags.write_rate, flags.op_rate, flags.duration, flags.trace_sample,
        flags.cache_mb, flags.write_buffer_mb, flags.max_file_mb, flags.block_kb,
        flags.read_delay_us, flags.seed);
    std::fclose(meta);
  }

  const std::string csv_path = flags.out_prefix + ".timeseries.csv";
  std::FILE* csv = std::fopen(csv_path.c_str(), "w");
  if (csv == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", csv_path.c_str());
    return 1;
  }
  std::fprintf(csv,
      "t,qps,read_qps,write_qps,block_lookups,block_hits,hit_ratio,"
      "read_p50_us,read_p95_us,read_p99_us,write_p99_us,"
      "compactions_running,compactions_done,flushes_done,trivial_moves,"
      "cache_live_mb,cache_stale_mb,stale_ids,sst_files,cache_charge_mb\n");

  // Turn on the emulated device delay only now: the load above must not pay it.
  ReadDelayEnabled().store(true, std::memory_order_relaxed);
  shared.run_start_us = env->NowMicros();
  if (adapter != nullptr) adapter->ResetClock();
  shared.measure_start_us = shared.run_start_us +
      static_cast<uint64_t>(flags.warmup) * 1000000ULL;
  shared.running.store(true);
  std::vector<std::thread> workers;
  for (int i = 0; i < flags.threads; ++i) workers.emplace_back(WorkerLoop, &shared, i);

  std::fprintf(stderr, "[run] warmup %ds, measure %ds, %d threads\n",
               flags.warmup, flags.duration, flags.threads);

  const int hist_n = Histogram::kNumBuckets;
  std::vector<uint64_t> rprev(hist_n, 0), wprev(hist_n, 0);
  uint64_t prev_reads = 0, prev_writes = 0, prev_lookups = 0, prev_hits = 0;
  uint64_t prev_comp = 0, prev_flush = 0, prev_move = 0;
  uint64_t base_lookups = 0, base_hits = 0, base_inserts = 0, base_evictions = 0;
  uint64_t base_comp = 0, base_flush = 0, base_compacted_bytes = 0;

  const int total_secs = flags.warmup + flags.duration;
  for (int sec = 1; sec <= total_secs; ++sec) {
    if (sec == flags.warmup + 1) {
      shared.measuring.store(true);
      // Reset the per-second deltas so warmup does not leak into second 1.
      rprev.assign(hist_n, 0);
      wprev.assign(hist_n, 0);
      for (auto* h : shared.read_hist) h->AccumulateInto(&rprev);
      for (auto* h : shared.write_hist) h->AccumulateInto(&wprev);
      prev_reads = shared.reads.load();
      prev_writes = shared.writes.load();
      // Cache and background-op counters are cumulative from DB::Open, so the
      // warmup total must be subtracted out or it all lands in second 1.
      const CacheCounters warm = cache->Snapshot(0);
      prev_lookups = warm.lookups;
      prev_hits = warm.hits;
      base_lookups = warm.lookups;
      base_hits = warm.hits;
      base_inserts = warm.inserts;
      base_evictions = warm.evictions;
      prev_comp = base_comp = logger->compactions_done();
      prev_flush = base_flush = logger->flushes_done();
      prev_move = logger->trivial_moves();
      base_compacted_bytes = logger->compacted_bytes();
    }
    cache->SetClock(static_cast<uint64_t>(sec));
    if (adapter != nullptr) {
      adapter->set_qps(static_cast<double>(shared.reads.load() + shared.writes.load()) /
                       std::max(1, sec - flags.warmup));
    }
    env->SleepForMicroseconds(1000000);
    if (sec <= flags.warmup) continue;

    const uint64_t reads = shared.reads.load();
    const uint64_t writes = shared.writes.load();
    const CacheCounters cc = cache->Snapshot(static_cast<uint64_t>(flags.stale_after));
    std::vector<uint64_t> rcur(hist_n, 0), wcur(hist_n, 0);
    for (auto* h : shared.read_hist) h->AccumulateInto(&rcur);
    for (auto* h : shared.write_hist) h->AccumulateInto(&wcur);

    const uint64_t d_lookups = cc.lookups - prev_lookups;
    const uint64_t d_hits = cc.hits - prev_hits;
    const double hit_ratio = d_lookups > 0 ? static_cast<double>(d_hits) / d_lookups : 0.0;
    const uint64_t comp = logger->compactions_done();
    const uint64_t flush = logger->flushes_done();
    const uint64_t move = logger->trivial_moves();

    const int t = sec - flags.warmup;
    std::fprintf(csv,
        "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.5f,"
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%" PRIu64 ",%" PRIu64 ",%.2f\n",
        t, (reads - prev_reads) + (writes - prev_writes), reads - prev_reads,
        writes - prev_writes, d_lookups, d_hits, hit_ratio,
        Percentile(rcur, rprev, 0.50), Percentile(rcur, rprev, 0.95),
        Percentile(rcur, rprev, 0.99), Percentile(wcur, wprev, 0.99),
        logger->compactions_running(), comp - prev_comp, flush - prev_flush,
        move - prev_move, cc.live_bytes / 1048576.0, cc.stale_bytes / 1048576.0,
        cc.stale_ids, TotalFiles(db), cache->TotalCharge() / 1048576.0);
    std::fflush(csv);

    std::fprintf(stderr,
        "t=%3d qps=%7" PRIu64 " hit=%.4f p95=%5" PRIu64 "us p99=%6" PRIu64
        "us comp=%" PRIu64 " flush=%" PRIu64 " stale=%.1fMB/%.1fMB\n",
        t, (reads - prev_reads) + (writes - prev_writes), hit_ratio,
        Percentile(rcur, rprev, 0.95), Percentile(rcur, rprev, 0.99),
        comp - prev_comp, flush - prev_flush, cc.stale_bytes / 1048576.0,
        cc.live_bytes / 1048576.0);

    rprev.swap(rcur);
    wprev.swap(wcur);
    prev_reads = reads; prev_writes = writes;
    prev_lookups = cc.lookups; prev_hits = cc.hits;
    prev_comp = comp; prev_flush = flush; prev_move = move;
  }

  shared.running.store(false);
  for (auto& t : workers) t.join();
  for (auto* tw : shared.trace) { tw->Close(); delete tw; }
  std::fclose(csv);

  const std::string ev_path = flags.out_prefix + ".events.csv";
  std::FILE* ev = std::fopen(ev_path.c_str(), "w");
  std::fprintf(ev, "t,type,level,bytes\n");
  for (const BgEvent& e : logger->events()) {
    std::fprintf(ev, "%.3f,%s,%d,%lld\n", e.t_secs - flags.warmup,
                 EventTypeName(e.type), e.level, e.bytes);
  }
  std::fclose(ev);

  const CacheCounters cc = cache->Snapshot(static_cast<uint64_t>(flags.stale_after));
  std::fprintf(stderr,
      "\n[summary] measured window only\n"
      "[summary] block cache: %" PRIu64 " lookups, %" PRIu64 " hits (%.4f), "
      "%" PRIu64 " inserts, %" PRIu64 " evictions\n"
      "[summary] background: %" PRIu64 " compactions, %" PRIu64 " flushes, "
      "%" PRIu64 " trivial moves, %.1f MB compacted\n"
      "[summary] wrote %s, %s, %s\n",
      cc.lookups - base_lookups, cc.hits - base_hits,
      (cc.lookups - base_lookups)
          ? static_cast<double>(cc.hits - base_hits) / (cc.lookups - base_lookups)
          : 0.0,
      cc.inserts - base_inserts, cc.evictions - base_evictions,
      logger->compactions_done() - base_comp, logger->flushes_done() - base_flush,
      logger->trivial_moves(),
      (logger->compacted_bytes() - base_compacted_bytes) / 1048576.0,
      csv_path.c_str(), ev_path.c_str(), log_path.c_str());

  if (adapter != nullptr) {
    const leaper::Stats ls = adapter->stats();
    std::fprintf(stderr,
        "[leaper] reads_seen=%" PRIu64 " writes_seen=%" PRIu64 " sampled=%" PRIu64 "\n"
        "[leaper] inferences=%" PRIu64 " inference_us=%" PRIu64 " (%.2f us/inf)\n"
        "[leaper] hot=%" PRIu64 " cold=%" PRIu64 " prefetched=%" PRIu64
        " evicted=%" PRIu64 " refused_budget=%" PRIu64 "\n"
        "[leaper] warm_calls=%" PRIu64 " warm_us=%" PRIu64 "\n",
        ls.reads_seen, ls.writes_seen, ls.sampled, ls.inferences, ls.inference_us,
        ls.inferences ? static_cast<double>(ls.inference_us) / ls.inferences : 0.0,
        ls.ranges_predicted_hot, ls.ranges_predicted_cold, ls.blocks_prefetched,
        ls.blocks_evicted, ls.prefetch_refused_budget,
        adapter->warmed_blocks(), adapter->warm_us());
  }
  for (auto* h : shared.read_hist) delete h;
  for (auto* h : shared.write_hist) delete h;
  delete db;
  delete opts.filter_policy;
  delete cache;   // also deletes the wrapped LRU cache
  delete logger;  // also closes the raw log sink
  delete pread_env;
  return 0;
}

}  // namespace
}  // namespace leaper_bench

int main(int argc, char** argv) {
  leaper_bench::ParseArgs(argc, argv);
  return leaper_bench::Run();
}
