// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).

#include "leaper_rocksdb.h"

#include "rocksdb/comparator.h"
#include "rocksdb/table.h"
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
// Remaining warm budget for the job whose End is running on this thread, in
// data blocks; 0 means unlimited. CacheOps::Prefetch has no budget parameter
// (the core interface is engine-neutral), and End and Prefetch always run on
// the same background thread, so a thread-local is the honest channel.
static thread_local uint64_t tl_warm_budget_left = 0;

// For warm_mode "prepop". RocksDB fires OnFlushBegin / OnCompactionBegin on
// the thread that then builds the job's output tables (with subcompactions
// disabled, which the harness leaves at their default), so the ranges chosen
// at Begin are handed to the builder's filter through a thread-local rather
// than through a job id the builder does not have. Sorted range ids, plus
// how many blocks this job may still warm (0 = unlimited).
struct PrepopJob {
  std::vector<leaper::RangeId> hot;
  uint64_t budget_left = 0;
  bool active = false;
};
static thread_local PrepopJob tl_prepop_job;

#if LEAPER_HAVE_PREPOP_FILTER
// The filter RocksDB's patched builder consults for every data block it is
// about to warm: yes iff the block's key span overlaps a chosen range of the
// job running on this thread, and the job's budget is not spent.
class Adapter::PrepopFilter : public rocksdb::PrepopulateBlockFilter {
 public:
  explicit PrepopFilter(Adapter* a) : a_(a) {}
  bool ShouldWarm(rocksdb::TableFileCreationReason,
                  const rocksdb::Slice& first_ikey,
                  const rocksdb::Slice& last_ikey) override {
    PrepopJob& job = tl_prepop_job;
    if (!job.active || job.hot.empty()) return Reject();
    if (job.budget_left == 0 && a_->warm_block_budget_ != 0) return Reject();
    // Internal key = user key + 8-byte trailer.
    if (first_ikey.size() < 8 || last_ikey.size() < 8) return Reject();
    leaper::RangeId lo = a_->mapper_->Map(first_ikey.data(), first_ikey.size() - 8);
    leaper::RangeId hi = a_->mapper_->Map(last_ikey.data(), last_ikey.size() - 8);
    if (hi < lo) std::swap(lo, hi);
    auto it = std::lower_bound(job.hot.begin(), job.hot.end(), lo);
    if (it == job.hot.end() || *it > hi) return Reject();
    if (a_->warm_block_budget_ != 0) --job.budget_left;
    std::lock_guard<std::mutex> lock(a_->mu_);
    ++a_->warmed_blocks_;
    return true;
  }
 private:
  bool Reject() {
    std::lock_guard<std::mutex> lock(a_->mu_);
    ++a_->prepop_rejected_;
    return false;
  }
  Adapter* a_;
};
#endif  // LEAPER_HAVE_PREPOP_FILTER

class Adapter::CacheBridge : public leaper::CacheOps {
 public:
  explicit CacheBridge(Adapter* a) : a_(a) {}

  // Data blocks this thread has read from disk, from the thread-local
  // PerfContext the harness already enables.
  static uint64_t DataBlocksRead() {
    const rocksdb::PerfContext* pc = rocksdb::get_perf_context();
    return pc->block_read_count - pc->index_block_read_count -
           pc->filter_block_read_count - pc->compression_dict_block_read_count;
  }

  void Evict(const leaper::BlockRef&) override {}

  bool IsCached(const leaper::BlockRef&) override { return false; }

  void Prefetch(const leaper::BlockRef& b) override {
    if (a_->db_ == nullptr) return;
    const uint64_t t0 = MonotonicUs();
    const uint64_t blocks0 = DataBlocksRead();
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
    bool stopped = false;
    auto over = [&] {
      return tl_warm_budget_left != 0 && DataBlocksRead() - blocks0 >= tl_warm_budget_left;
    };
    it->Seek(rocksdb::Slice(start));
    if (over()) stopped = true;  // the seek's own read may already exhaust it
    for (; !stopped && it->Valid() && n < a_->warm_scan_keys_; it->Next()) {
      ++n;
      // Checked per key, so a single scan cannot run a whole range past the
      // budget; the overshoot is at most the block the last key landed in.
      if (over()) {
        stopped = true;
        break;
      }
    }
    const uint64_t read = DataBlocksRead() - blocks0;
    if (tl_warm_budget_left != 0) tl_warm_budget_left -= std::min(tl_warm_budget_left, read);
    std::lock_guard<std::mutex> lock(a_->mu_);
    a_->warm_us_ += MonotonicUs() - t0;
    a_->warmed_blocks_ += read;
    ++a_->warmed_;
    if (stopped) ++a_->warm_budget_stops_;
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
  if (a_->warm_mode_ == "prepop") {
    PrepopJob& job = tl_prepop_job;
    job.hot.clear();
    for (const leaper::BlockRef& b : chosen) job.hot.push_back(b.first_range);
    std::sort(job.hot.begin(), job.hot.end());
    job.budget_left = a_->warm_block_budget_;
    job.active = true;
    a_->warmed_ += chosen.size();
    return;  // nothing to do at End: the builder warms as it writes
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
  //
  // The core's byte budget cannot see these warms: the candidates this
  // adapter builds are whole key ranges with size 0, so ShouldPrefetch's
  // check never fires. The budget is enforced here instead, per job and
  // while the reads happen, by counting the data blocks this thread's
  // PerfContext says each warm pulled in. Without it the iterator mode was
  // unbounded (3,350 s of background warming in one 200 s run), and a check
  // made only before the first read let the sst mode read a whole file's
  // worth past a budget of one block.
  const uint64_t budget = a_->warm_block_budget_;  // 0 = unlimited
  if (a_->warm_mode_ == "prepop") {
    tl_prepop_job.active = false;
    tl_prepop_job.hot.clear();
  } else if (a_->warm_mode_ == "sst" && a_->table_factory_ != nullptr) {
    a_->WarmFromFiles(outputs, chosen, budget);
  } else {
    tl_warm_budget_left = budget;
    for (const leaper::BlockRef& b : chosen) {
      if (budget != 0 && tl_warm_budget_left == 0) break;  // Prefetch counted the stop
      a_->bridge_->Prefetch(b);
    }
    tl_warm_budget_left = 0;
  }
  // The core's job context is a stack shared by every job (see the core's
  // OnCompactionBegin). Begin runs its predict-and-choose sequence under
  // this adapter's mutex so that no other job's Begin interleaves with it;
  // End's pop has to take the same mutex, or it can restore an older context
  // in the middle of another job's choosing and that job then selects
  // against a stale hot set (review follow-up, 2026-09-20, item B). The warm
  // I/O above stays outside the lock.
  leaper::CompactionInfo info;
  std::lock_guard<std::mutex> lock(a_->mu_);
  a_->core_->OnCompactionEnd(info, a_->NowUs());
}

bool Adapter::PrepopSupported() {
#if LEAPER_HAVE_PREPOP_FILTER
  return true;
#else
  return false;
#endif
}

#if LEAPER_HAVE_PREPOP_FILTER
std::shared_ptr<rocksdb::PrepopulateBlockFilter> Adapter::prepopulate_filter() {
  if (prepop_filter_ == nullptr) prepop_filter_ = std::make_shared<PrepopFilter>(this);
  return prepop_filter_;
}
#endif

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
                            const std::vector<leaper::BlockRef>& ranges,
                            uint64_t budget_blocks) {
  if (ranges.empty() || outputs.empty()) return;
  const uint64_t t0 = MonotonicUs();
  rocksdb::Options o;
  o.table_factory = table_factory_;
  o.comparator = comparator_ != nullptr ? comparator_ : rocksdb::BytewiseComparator();
  rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableCount);
  rocksdb::get_perf_context()->Reset();
  auto blocks_so_far = [] {
    const rocksdb::PerfContext* pc = rocksdb::get_perf_context();
    return pc->block_read_count - pc->index_block_read_count -
           pc->filter_block_read_count - pc->compression_dict_block_read_count;
  };
  uint64_t blocks = 0, files = 0, failed = 0, stops = 0;
  for (const std::string& path : outputs) {
    if (budget_blocks != 0 && blocks_so_far() >= budget_blocks) { ++stops; break; }
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
    bool exhausted = false;
    auto over = [&] { return budget_blocks != 0 && blocks_so_far() >= budget_blocks; };
    for (const leaper::BlockRef& b : ranges) {
      const std::string start = mapper_->RangeStartKey(b.first_range);
      const std::string limit = mapper_->RangeStartKey(b.last_range + 1);
      // The budget is checked around every read the scan can make, not only
      // per key inside the loop. A Seek reads the block it lands in whether
      // or not that block holds a key of this range, and a predicted range
      // that has no keys in this particular output file -- normal, since
      // prediction is over the whole key space and each file covers a slice
      // of it -- never enters the loop body at all. Checking only there let
      // a file of alternately empty ranges read 355 blocks past a budget of
      // 32 (review round 3). So: check before the Seek, right after it, and
      // per key. A check between ranges only, as first written, let a
      // single 40,000-key range read 345 blocks past a budget of 32.
      if (over()) { exhausted = true; break; }
      it->Seek(rocksdb::Slice(start));
      if (over()) { exhausted = true; break; }
      for (; it->Valid() && o.comparator->Compare(it->key(), rocksdb::Slice(limit)) < 0;
           it->Next()) {
        // Reading is the point: each new block the iterator enters is one
        // fill_cache insert under the key the DB's reader will use.
        if (over()) { exhausted = true; break; }
      }
      if (exhausted) break;
    }
    if (exhausted) { ++stops; break; }
  }
  blocks = blocks_so_far();
  std::lock_guard<std::mutex> lock(mu_);
  warm_us_ += MonotonicUs() - t0;
  warmed_ += ranges.size();
  warmed_blocks_ += blocks;
  warm_budget_stops_ += stops;
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
  if (a->warm_mode_ == "prepop" && !PrepopSupported()) {
    if (error) {
      *error = "warm_mode=prepop needs the RocksDB prepopulate-filter patch "
               "(adapters/rocksdb/rocksdb-11.8-prepopulate-filter.patch); this build "
               "was configured against a RocksDB without it";
    }
    return nullptr;
  }
  a->warm_block_budget_ = static_cast<uint64_t>(
      opts.core.max_prefetch_frac * opts.core.cache_bytes / opts.warm_block_bytes);
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
