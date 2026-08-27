// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Lock-free, two-level latency histogram: exact 1us resolution below 64us,
// ~3% relative resolution above (5 mantissa bits, 32 buckets per octave).
// Each worker thread owns one instance; the reporter thread reads it
// concurrently via relaxed atomics, so snapshots are eventually-consistent
// but never torn.

#ifndef LEAPER_BENCH_HISTOGRAM_H_
#define LEAPER_BENCH_HISTOGRAM_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace leaper_bench {

class Histogram {
 public:
  static constexpr int kSubBucketBits = 5;                  // 32 buckets/octave
  static constexpr int kSubBuckets = 1 << kSubBucketBits;   // 32
  static constexpr int kLinearMax = 2 * kSubBuckets;        // 64us exact
  static constexpr int kOctaves = 34;                       // up to ~2^40 us
  static constexpr int kNumBuckets = kLinearMax + kOctaves * kSubBuckets;

  Histogram() : buckets_(kNumBuckets) {
    for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
  }

  static int BucketOf(uint64_t us) {
    if (us < kLinearMax) return static_cast<int>(us);
    const int e = 63 - __builtin_clzll(us);
    const int m = static_cast<int>((us >> (e - kSubBucketBits)) & (kSubBuckets - 1));
    const int idx = kLinearMax + (e - (kSubBucketBits + 1)) * kSubBuckets + m;
    return std::min(idx, kNumBuckets - 1);
  }

  static uint64_t ValueOf(int bucket) {
    if (bucket < kLinearMax) return static_cast<uint64_t>(bucket);
    const int rel = bucket - kLinearMax;
    const int e = kSubBucketBits + 1 + rel / kSubBuckets;
    const int m = rel % kSubBuckets;
    return (static_cast<uint64_t>(kSubBuckets + m)) << (e - kSubBucketBits);
  }

  void Add(uint64_t us) {
    buckets_[BucketOf(us)].fetch_add(1, std::memory_order_relaxed);
  }

  // Adds this histogram's current counts into |out| (size kNumBuckets).
  void AccumulateInto(std::vector<uint64_t>* out) const {
    for (int i = 0; i < kNumBuckets; ++i) {
      (*out)[i] += buckets_[i].load(std::memory_order_relaxed);
    }
  }

 private:
  std::vector<std::atomic<uint64_t>> buckets_;
};

// Percentile over the difference between two cumulative snapshots.
inline uint64_t Percentile(const std::vector<uint64_t>& cur,
                           const std::vector<uint64_t>& prev, double p) {
  uint64_t total = 0;
  for (size_t i = 0; i < cur.size(); ++i) total += cur[i] - prev[i];
  if (total == 0) return 0;
  const uint64_t target = static_cast<uint64_t>(total * p);
  uint64_t seen = 0;
  for (size_t i = 0; i < cur.size(); ++i) {
    seen += cur[i] - prev[i];
    if (seen >= target) return Histogram::ValueOf(static_cast<int>(i));
  }
  return Histogram::ValueOf(static_cast<int>(cur.size()) - 1);
}

}  // namespace leaper_bench

#endif  // LEAPER_BENCH_HISTOGRAM_H_
