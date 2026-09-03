// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).

#include "leaper_rocksdb.h"

#include "rocksdb/comparator.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/perf_level.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/table.h"

#include <algorithm>
#include <chrono>

#include "rocksdb/iterator.h"
#include "rocksdb/metadata.h"
#include "rocksdb/options.h"

namespace leaper_rocksdb {
namespace {

uint64_t MonotonicUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

// Warms a predicted-hot key range by seeking to its first key and scanning
// forward with fill_cache on, which pulls exactly the blocks covering that
// range into the block cache under the keys the read path will use.
//
// Evict is a no-op: RocksDB block cache keys are derived from a per-file
// OffsetableCacheKey held inside the table reader, so a plug-in cannot address
// them. Phase 1 is therefore absent on RocksDB, and the results report Leaper
// there as prefetch-only.
class Adapter::CacheBridge : public leaper::CacheOps {
 public:
  explicit CacheBridge(Adapter* a) : a_(a) {}

  void Evict(const leaper::BlockRef&) override {}

  bool IsCached(const leaper::BlockRef&) override { return false; }

  void Prefetch(const leaper::BlockRef& b) override {
    if (a_->db_ == nullptr) return;
    const uint64_t t0 = MonotonicUs();
    rocksdb::ReadOptions ro;
    ro.fill_cache = true;
    ro.verify_checksums = false;
    const std::string start = a_->mapper_->RangeStartKey(b.first_range);
    const std::string limit =
        a_->mapper_->RangeStartKey(b.last_range + 1);
    rocksdb::Slice upper(limit);
    ro.iterate_upper_bound = &upper;
    std::unique_ptr<rocksdb::Iterator> it(a_->db_->NewIterator(ro));
    int n = 0;
    for (it->Seek(rocksdb::Slice(start)); it->Valid() && n < a_->warm_scan_keys_;
         it->Next()) {
      ++n;
    }
    std::lock_guard<std::mutex> lock(a_->mu_);
    a_->warm_us_ += MonotonicUs() - t0;
    ++a_->warmed_;
  }

 private:
  Adapter* a_;
};

// RocksDB gives compaction visibility out of the box; this is the whole
// reason the RocksDB integration needs no core patch while LevelDB did.
class Adapter::Listener : public rocksdb::EventListener {
 public:
  explicit Listener(Adapter* a) : a_(a) {}

  void OnFlushBegin(rocksdb::DB*, const rocksdb::FlushJobInfo& info) override {
    Begin(info.job_id, /*level=*/0, /*is_flush=*/true, info.smallest_seqno, 0);
  }
  void OnFlushCompleted(rocksdb::DB*, const rocksdb::FlushJobInfo& info) override {
    End(info.job_id, {info.file_path});
  }
  void OnCompactionBegin(rocksdb::DB* db,
                         const rocksdb::CompactionJobInfo& info) override {
    // Estimate the block count from the input bytes; the paper's T1 estimate
    // is linear in blocks merged and this is the closest RocksDB exposes
    // without reading the inputs.
    uint64_t bytes = 0;
    for (const auto& kv : info.table_properties) bytes += kv.second->data_size;
    Begin(info.job_id, info.base_input_level, /*is_flush=*/false, 0, bytes / 4096);
    (void)db;
  }
  void OnCompactionCompleted(rocksdb::DB*,
                             const rocksdb::CompactionJobInfo& info) override {
    End(info.job_id, info.output_files);
  }

 private:
  void Begin(int job_id, int level, bool is_flush, uint64_t, uint64_t est_blocks);
  void End(int job_id, const std::vector<std::string>& outputs);
  Adapter* a_;
};

void Adapter::Listener::Begin(int job_id, int level, bool is_flush, uint64_t,
                              uint64_t est_blocks) {
  // The core keeps one job's prediction at a time (hot_t2_), so predicting
  // and choosing for this job must not interleave with another job's Begin.
  std::lock_guard<std::mutex> lock(a_->mu_);
  // Candidates are every range the database currently spans. RocksDB does not
  // hand a plug-in the block layout of the inputs, so the prediction is made
  // over the whole range space rather than only over the blocks being
  // rewritten. That is a superset, and the model's job is to cut it down.
  std::vector<leaper::BlockRef> candidates;
  const uint64_t n_ranges = a_->NumRanges();
  candidates.reserve(n_ranges);
  for (uint64_t r = 0; r < n_ranges; ++r) {
    leaper::BlockRef b;
    b.file_id = 0;
    b.offset = r;
    b.size = 0;
    b.first_range = r;
    b.last_range = r;
    candidates.push_back(b);
  }
  leaper::CompactionInfo info;
  info.level = level;
  info.is_flush = is_flush;
  info.est_blocks = est_blocks;
  a_->core_->OnCompactionBegin(info, candidates, a_->NowUs());

  // Decide now, warm later. The first version of this adapter warmed here, at
  // compaction *begin* -- before the output files existed -- so every seek hit
  // the input files and pulled in exactly the blocks the compaction was about
  // to invalidate. The M7 numbers measured with that version showed Leaper at
  // +0.00pp with a tripled p99: all of the cost, none of the benefit.
  std::vector<leaper::BlockRef> chosen;
  for (const leaper::BlockRef& b : candidates) {
    if (a_->core_->ShouldPrefetch(b, a_->NowUs())) chosen.push_back(b);
  }
  a_->pending_by_job_[job_id] = std::move(chosen);
}

void Adapter::Listener::End(int job_id, const std::vector<std::string>& outputs) {
  std::vector<leaper::BlockRef> chosen;
  {
    std::lock_guard<std::mutex> lock(a_->mu_);
    auto it = a_->pending_by_job_.find(job_id);
    if (it != a_->pending_by_job_.end()) {
      chosen = std::move(it->second);
      a_->pending_by_job_.erase(it);
    }
  }
  // The new files are installed and readable; warming now lands on them.
  if (a_->warm_mode_ == "sst" && a_->table_factory_ != nullptr) {
    a_->WarmFromFiles(outputs, chosen);
  } else {
    for (const leaper::BlockRef& b : chosen) a_->bridge_->Prefetch(b);
  }
  leaper::CompactionInfo info;
  a_->core_->OnCompactionEnd(info, a_->NowUs());
}

void Adapter::SetTableFactory(std::shared_ptr<rocksdb::TableFactory> factory,
                              const rocksdb::Comparator* comparator) {
  table_factory_ = std::move(factory);
  comparator_ = comparator;
}

// Block-level warming of exactly the job's output: open each output file
// with a reader that shares the DB's block cache and pull in the data blocks
// that lie inside a predicted-hot range. Runs on the background thread that
// finished the job, so its reads are not the workload's (the harness counts
// them under bg_lookups) and its cost is charged where the LevelDB hook's is.
void Adapter::WarmFromFiles(const std::vector<std::string>& outputs,
                            const std::vector<leaper::BlockRef>& ranges) {
  if (ranges.empty() || outputs.empty()) return;
  const uint64_t t0 = MonotonicUs();
  rocksdb::Options o;
  o.table_factory = table_factory_;
  o.comparator = comparator_ != nullptr ? comparator_ : rocksdb::BytewiseComparator();
  rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableCount);
  rocksdb::get_perf_context()->Reset();
  uint64_t blocks = 0, files = 0, failed = 0;
  for (const std::string& path : outputs) {
    rocksdb::SstFileReader reader(o);
    if (!reader.Open(path).ok()) {
      ++failed;
      continue;
    }
    ++files;
    rocksdb::ReadOptions ro;
    ro.fill_cache = true;
    ro.verify_checksums = false;
    std::unique_ptr<rocksdb::Iterator> it(reader.NewIterator(ro));
    for (const leaper::BlockRef& b : ranges) {
      const std::string start = mapper_->RangeStartKey(b.first_range);
      const std::string limit = mapper_->RangeStartKey(b.last_range + 1);
      for (it->Seek(rocksdb::Slice(start));
           it->Valid() && o.comparator->Compare(it->key(), rocksdb::Slice(limit)) < 0;
           it->Next()) {
        // Reading is the point: each new block the iterator enters is one
        // fill_cache insert under the key the DB's reader will use.
      }
    }
  }
  const rocksdb::PerfContext* pc = rocksdb::get_perf_context();
  blocks = pc->block_read_count - pc->index_block_read_count - pc->filter_block_read_count;
  std::lock_guard<std::mutex> lock(mu_);
  warm_us_ += MonotonicUs() - t0;
  warmed_ += ranges.size();
  warmed_blocks_ += blocks;
  warm_files_ += files;
  warm_open_failed_ += failed;
}

// ---------------------------------------------------------------------------

std::unique_ptr<Adapter> Adapter::Create(const AdapterOptions& opts,
                                         std::string* error) {
  std::unique_ptr<Adapter> a(new Adapter());
  a->start_us_ = MonotonicUs();
  a->warm_scan_keys_ = opts.warm_scan_keys;
  a->warm_mode_ = opts.warm_mode;
  a->range_size_ = opts.core.range_size ? opts.core.range_size : 1;
  a->mapper_ = (opts.key_format == "prefix")
                   ? leaper::NewPrefixRangeMapper(opts.core.range_size)
                   : leaper::NewDecimalRangeMapper(opts.core.range_size);
  a->bridge_.reset(new CacheBridge(a.get()));
  a->core_ = leaper::Leaper::Open(opts.core, a->mapper_.get(), a->bridge_.get(),
                                  error);
  if (a->core_ == nullptr) return nullptr;
  a->listener_ = std::make_shared<Listener>(a.get());
  a->num_ranges_ = opts.num_ranges;
  return a;
}

Adapter::~Adapter() = default;

std::shared_ptr<rocksdb::EventListener> Adapter::listener() { return listener_; }
void Adapter::SetDB(rocksdb::DB* db) { db_ = db; }
void Adapter::ResetClock() { start_us_ = MonotonicUs(); }
uint64_t Adapter::NowUs() const { return MonotonicUs() - start_us_; }
uint64_t Adapter::NumRanges() const { return num_ranges_; }

void Adapter::OnRead(const rocksdb::Slice& key) {
  core_->OnRead(key.data(), key.size(), NowUs());
}
void Adapter::OnWrite(const rocksdb::Slice& key) {
  core_->OnWrite(key.data(), key.size(), NowUs());
}
void Adapter::set_qps(double qps) { core_->set_qps(qps); }
void Adapter::set_health(double m) { core_->set_health(m); }
leaper::Stats Adapter::stats() const { return core_->stats(); }
uint64_t Adapter::warmed_ranges() const {
  std::lock_guard<std::mutex> lock(mu_);
  return warmed_;
}
uint64_t Adapter::warm_us() const {
  std::lock_guard<std::mutex> lock(mu_);
  return warm_us_;
}

}  // namespace leaper_rocksdb
