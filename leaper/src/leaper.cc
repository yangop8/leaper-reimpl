// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// The plug-in itself: range mappers, the policy switch, and the two-phase
// prefetcher (paper Section 6.2).

#include "leaper/leaper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include "collector.h"
#include "overlap.h"
#include "predictor.h"

namespace leaper {

namespace {
thread_local int g_internal_depth = 0;
}  // namespace

bool InInternalCacheAccess() { return g_internal_depth > 0; }
ScopedInternalCacheAccess::ScopedInternalCacheAccess() { ++g_internal_depth; }
ScopedInternalCacheAccess::~ScopedInternalCacheAccess() { --g_internal_depth; }

const char* PolicyName(Policy p) {
  switch (p) {
    case Policy::kOff: return "off";
    case Policy::kEagerEvict: return "eager_evict";
    case Policy::kIncrementalWarmup: return "incremental_warmup";
    case Policy::kWarmAll: return "warm_all";
    case Policy::kLeaper: return "leaper";
    case Policy::kOracle: return "oracle";
  }
  return "unknown";
}

bool ParsePolicy(const std::string& name, Policy* out) {
  if (name == "off") *out = Policy::kOff;
  else if (name == "eager_evict") *out = Policy::kEagerEvict;
  else if (name == "incremental_warmup") *out = Policy::kIncrementalWarmup;
  else if (name == "warm_all") *out = Policy::kWarmAll;
  else if (name == "leaper") *out = Policy::kLeaper;
  else if (name == "oracle") *out = Policy::kOracle;
  else return false;
  return true;
}

namespace {

class PrefixRangeMapper : public RangeMapper {
 public:
  explicit PrefixRangeMapper(uint64_t range_size)
      : range_size_(range_size ? range_size : 1) {}
  RangeId Map(const char* key, size_t len) const override {
    uint64_t v = 0;
    const size_t n = std::min<size_t>(len, 8);
    for (size_t i = 0; i < n; ++i) {
      v = (v << 8) | static_cast<unsigned char>(key[i]);
    }
    for (size_t i = n; i < 8; ++i) v <<= 8;  // right-pad: keeps order
    return v / range_size_;
  }
  uint64_t range_size() const override { return range_size_; }
  std::string RangeStartKey(RangeId range) const override {
    const uint64_t v = range * range_size_;
    std::string k(8, '\0');
    for (int i = 0; i < 8; ++i) k[i] = static_cast<char>((v >> (56 - 8 * i)) & 0xff);
    return k;
  }
 private:
  uint64_t range_size_;
};

class DecimalRangeMapper : public RangeMapper {
 public:
  DecimalRangeMapper(uint64_t range_size, int key_width)
      : range_size_(range_size ? range_size : 1),
        key_width_(key_width > 0 ? key_width : 16) {}
  RangeId Map(const char* key, size_t len) const override {
    uint64_t v = 0;
    int digits = 0;
    for (size_t i = 0; i < len; ++i) {
      const char c = key[i];
      if (c < '0' || c > '9') break;
      v = v * 10 + static_cast<uint64_t>(c - '0');
      ++digits;
    }
    // Restore a key shortened by FindShortestSeparator to its full width; see
    // NewDecimalRangeMapper's comment for why this is exact.
    for (int i = digits; i < key_width_; ++i) v *= 10;
    return v / range_size_;
  }
  uint64_t range_size() const override { return range_size_; }
  std::string RangeStartKey(RangeId range) const override {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%0*llu", key_width_,
                  static_cast<unsigned long long>(range * range_size_));
    return std::string(buf, static_cast<size_t>(key_width_));
  }
 private:
  uint64_t range_size_;
  int key_width_;
};

// ---------------------------------------------------------------------------

class LeaperImpl : public Leaper {
 public:
  LeaperImpl(const Options& o, RangeMapper* mapper, CacheOps* cache)
      : opt_(o), mapper_(mapper), cache_(cache),
        collector_(o.max_ranges, o.history_slots, o.slot_seconds, o.sample_rate) {}

  bool Init(std::string* error) {
    if (opt_.policy != Policy::kLeaper) return true;
    if (opt_.model_paths.empty()) {
      if (error) *error = "policy=leaper requires at least one model path";
      return false;
    }
    return predictor_.Load(opt_.model_paths, opt_.precursor_path, opt_.history_slots,
                           3, error);
  }

  bool LoadOracle(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
      if (error) *error = "cannot open oracle file: " + path;
      return false;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ls(line);
      uint64_t slot;
      if (!(ls >> slot)) continue;
      RangeId r;
      auto& v = oracle_[slot];
      while (ls >> r) v.push_back(r);
      std::sort(v.begin(), v.end());
    }
    return true;
  }

  void OnRead(const char* key, size_t len, uint64_t now_us) override {
    if (!Collecting()) return;
    collector_.Record(mapper_->Map(key, len), false, now_us);
  }

  void OnWrite(const char* key, size_t len, uint64_t now_us) override {
    if (!Collecting()) return;
    collector_.Record(mapper_->Map(key, len), true, now_us);
  }

  void OnCompactionBegin(const CompactionInfo& info,
                         const std::vector<BlockRef>& input_blocks,
                         uint64_t now_us) override {
    std::lock_guard<std::mutex> lock(mu_);
    hot_t2_.clear();
    prefetched_bytes_ = 0;
    budget_bytes_ = static_cast<uint64_t>(opt_.max_prefetch_frac * opt_.cache_bytes);

    switch (opt_.policy) {
      case Policy::kOff:
      case Policy::kEagerEvict:
        return;

      case Policy::kWarmAll:
        // Warm everything the compaction writes; nothing to decide up front.
        return;

      case Policy::kIncrementalWarmup: {
        // The paper's baseline: newly compacted blocks are assumed hot if they
        // overlap blocks that are in the cache now. The cached input blocks are
        // evicted as the new ones take their place.
        std::vector<BlockRef> live;
        for (const BlockRef& b : input_blocks) {
          if (cache_->IsCached(b)) live.push_back(b);
        }
        std::vector<RangeId> cached;
        ExpandRanges(live, &cached);
        hot_t2_ = ToSpans(cached);
        for (const BlockRef& b : input_blocks) {
          if (cache_->IsCached(b)) {
            cache_->Evict(b);
            ++stats_.blocks_evicted;
          }
        }
        return;
      }

      case Policy::kOracle: {
        const uint64_t slot = collector_.SlotOf(now_us) + 1;
        auto it = oracle_.find(slot);
        if (it != oracle_.end()) hot_t2_ = ToSpans(it->second);
        return;
      }

      case Policy::kLeaper:
        break;
    }

    // --- Leaper: multi-step prediction combined into two phases. -----------
    // T1 is how long the compaction will run, T2 how long the cache takes to
    // recover afterwards (paper eq. 3). Both are linear estimates whose
    // constants are calibrated offline.
    const double qps = qps_.load(std::memory_order_relaxed);
    const double t1 = opt_.t1_seconds > 0.0
                          ? opt_.t1_seconds
                          : opt_.t1_alpha * static_cast<double>(info.est_blocks);
    const double t2 = opt_.t2_seconds > 0.0
                          ? opt_.t2_seconds
                          : opt_.t2_beta * (qps / std::max(1.0, opt_.cache_bytes));
    const int k1 = std::max(1, static_cast<int>(std::ceil(t1 / opt_.slot_seconds)));
    const int k2 = std::max(1, static_cast<int>(std::ceil(t2 / opt_.slot_seconds)));

    std::vector<RangeId> candidates;
    ExpandRanges(input_blocks, &candidates);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    const uint64_t t_infer0 = NowUs();
    std::vector<RangeId> hot1, hot2;
    if (info.is_flush) {
      // A flush is short: the paper predicts its accesses directly in one
      // phase rather than splitting into eviction and prefetch.
      predictor_.PredictHot(collector_, candidates, 1, k2, now_us,
                            opt_.hot_threshold, &hot2, &stats_.inferences);
    } else {
      predictor_.PredictHot(collector_, candidates, 1, k1, now_us,
                            opt_.hot_threshold, &hot1, &stats_.inferences);
      predictor_.PredictHot(collector_, candidates, k1 + 1, k1 + k2, now_us,
                            opt_.hot_threshold, &hot2, &stats_.inferences);
    }
    stats_.inference_us += NowUs() - t_infer0;
    stats_.ranges_predicted_hot += hot2.size();
    stats_.ranges_predicted_cold +=
        candidates.size() > hot2.size() ? candidates.size() - hot2.size() : 0;

    hot_t2_ = ToSpans(hot2);

    if (!info.is_flush && opt_.enable_phase1) {
      // Phase 1. The input SSTs stay readable until the new version is
      // installed, so blocks predicted hot for the duration of the compaction
      // must be kept; the rest are evicted now to give the cache back to data
      // that will actually be read. Only evicting predicted-cold blocks is
      // what keeps this safe: those are blocks LRU would have dropped anyway.
      const std::vector<RangeSpan> keep = ToSpans(hot1);
      const uint64_t t0 = NowUs();
      for (const BlockRef& b : input_blocks) {
        if (!BlockOverlaps(b, keep)) {
          cache_->Evict(b);
          ++stats_.blocks_evicted;
        }
      }
      stats_.overlap_us += NowUs() - t0;
      ++stats_.overlap_checks;
    }
  }

  bool ShouldPrefetch(const BlockRef& block, uint64_t /*now_us*/) override {
    std::lock_guard<std::mutex> lock(mu_);
    bool want = false;
    switch (opt_.policy) {
      case Policy::kOff:
      case Policy::kEagerEvict:
        return false;
      case Policy::kWarmAll:
        want = true;
        break;
      case Policy::kIncrementalWarmup:
      case Policy::kOracle:
        want = BlockOverlaps(block, hot_t2_);
        break;
      case Policy::kLeaper:
        want = opt_.enable_phase2 && BlockOverlaps(block, hot_t2_);
        break;
    }
    if (!want) return false;
    if (prefetched_bytes_ + block.size > budget_bytes_) {
      ++stats_.prefetch_refused_budget;
      return false;
    }
    prefetched_bytes_ += block.size;
    ++stats_.blocks_prefetched;
    return true;
  }

  void OnCompactionEnd(const CompactionInfo&, uint64_t) override {
    std::lock_guard<std::mutex> lock(mu_);
    hot_t2_.clear();
  }

  void OnFileObsolete(uint64_t, const std::vector<BlockRef>& blocks) override {
    // LevelDB never reclaims the block cache entries of an SST that compaction
    // deleted: TableCache::Evict drops the Table, but the data blocks keep
    // sitting under a cache_id that Table::Open will never hand out again, so
    // they are unreachable garbage until LRU happens to evict them. Reclaiming
    // them is free and has nothing to do with learning, which is exactly why
    // it is a separate baseline rather than part of Leaper.
    if (opt_.policy == Policy::kOff) return;
    std::lock_guard<std::mutex> lock(mu_);
    for (const BlockRef& b : blocks) {
      cache_->Evict(b);
      ++stats_.blocks_evicted;
    }
  }

  Stats stats() const override {
    std::lock_guard<std::mutex> lock(mu_);
    Stats s = stats_;
    s.reads_seen = collector_.reads_seen();
    s.writes_seen = collector_.writes_seen();
    s.sampled = collector_.sampled();
    return s;
  }

  void set_qps(double q) override { qps_.store(q, std::memory_order_relaxed); }

 private:
  bool Collecting() const {
    return opt_.policy == Policy::kLeaper;
  }

  // Turns block key-bound spans into the individual range ids the predictor
  // scores, clamped to the configured key space and hard-capped. Both guards
  // are load-bearing: a single malformed bound would otherwise allocate
  // without limit (see Options::max_range_id).
  void ExpandRanges(const std::vector<BlockRef>& blocks,
                    std::vector<RangeId>* out) const {
    const RangeId cap = opt_.max_range_id;
    for (const BlockRef& b : blocks) {
      const RangeId lo = std::min(b.first_range, cap);
      const RangeId hi = std::min(b.last_range, cap);
      for (RangeId r = lo; r <= hi; ++r) {
        out->push_back(r);
        if (out->size() > kMaxCandidates) break;
      }
      if (out->size() > kMaxCandidates) break;
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
  }

  static constexpr size_t kMaxCandidates = 1u << 20;
  static uint64_t NowUs();

  Options opt_;
  RangeMapper* mapper_;
  CacheOps* cache_;
  Collector collector_;
  Predictor predictor_;
  std::unordered_map<uint64_t, std::vector<RangeId>> oracle_;

  mutable std::mutex mu_;
  std::vector<RangeSpan> hot_t2_;
  uint64_t prefetched_bytes_ = 0, budget_bytes_ = 0;
  Stats stats_;
  std::atomic<double> qps_{0.0};
};

uint64_t LeaperImpl::NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

std::unique_ptr<RangeMapper> NewPrefixRangeMapper(uint64_t range_size) {
  return std::unique_ptr<RangeMapper>(new PrefixRangeMapper(range_size));
}
std::unique_ptr<RangeMapper> NewDecimalRangeMapper(uint64_t range_size,
                                                   int key_width) {
  return std::unique_ptr<RangeMapper>(
      new DecimalRangeMapper(range_size, key_width));
}

std::unique_ptr<Leaper> Leaper::Open(const Options& options, RangeMapper* mapper,
                                     CacheOps* cache, std::string* error) {
  std::unique_ptr<LeaperImpl> impl(new LeaperImpl(options, mapper, cache));
  if (!impl->Init(error)) return nullptr;
  if (options.policy == Policy::kOracle && !options.oracle_path.empty() &&
      !impl->LoadOracle(options.oracle_path, error)) {
    return nullptr;
  }
  return std::unique_ptr<Leaper>(impl.release());
}

}  // namespace leaper
