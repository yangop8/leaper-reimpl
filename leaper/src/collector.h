// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Statistics collection (paper Section 5.1).
//
// Per key range we keep a ring of |history+1| slots of read and write counts.
// Each cell packs (slot_epoch << 32 | count) into one atomic word, so a slot
// rolls over lazily on first touch and no background sweep is needed: the
// common path is a single relaxed fetch_add.
//
// Exactness is not required. The paper samples at P = 0.01 and measures a
// 16.3% error in the collected counts with negligible effect on prediction,
// because the model is a binary classifier over arrival-rate *shapes*. We
// therefore accept the rare lost increment at an epoch transition rather than
// pay for a CAS loop on every access.

#ifndef LEAPER_COLLECTOR_H_
#define LEAPER_COLLECTOR_H_

#include <atomic>
#include <cstdint>
#include <vector>

#include "leaper/leaper.h"

namespace leaper {

class Collector {
 public:
  Collector(size_t max_ranges, int history_slots, double slot_seconds,
            double sample_rate);

  void Record(RangeId range, bool is_write, uint64_t now_us);

  // Arrival rates for the |history| slots preceding the slot containing
  // |now_us|, oldest first -- the order the offline trainer emits
  // (read_rate_t-6 ... read_rate_t-1).
  void History(RangeId range, uint64_t now_us, float* reads, float* writes) const;

  uint64_t SlotOf(uint64_t now_us) const {
    return static_cast<uint64_t>(now_us / slot_us_);
  }

  int history_slots() const { return history_; }
  uint64_t reads_seen() const { return reads_seen_.load(std::memory_order_relaxed); }
  uint64_t writes_seen() const { return writes_seen_.load(std::memory_order_relaxed); }
  uint64_t sampled() const { return sampled_.load(std::memory_order_relaxed); }

 private:
  size_t Slot(RangeId r) const { return static_cast<size_t>(r) & mask_; }
  size_t Cell(size_t slot_index, uint64_t epoch) const {
    return slot_index * ring_ + static_cast<size_t>(epoch % ring_);
  }
  static uint32_t CountOf(uint64_t v, uint64_t epoch) {
    return (v >> 32) == epoch ? static_cast<uint32_t>(v & 0xffffffffu) : 0u;
  }
  // Paper's unbiased estimator for probability sampling where the first
  // access is always recorded: N = (S-1)/P + 1.
  float Estimate(uint32_t s) const {
    if (s == 0) return 0.0f;
    if (sample_rate_ >= 1.0) return static_cast<float>(s);
    return static_cast<float>((s - 1) / sample_rate_ + 1.0);
  }

  const int history_;
  const int ring_;              // history + 1
  const uint64_t slot_us_;
  const double sample_rate_;
  size_t mask_;

  std::vector<std::atomic<uint64_t>> reads_, writes_;
  std::atomic<uint64_t> reads_seen_{0}, writes_seen_{0}, sampled_{0};
  mutable std::atomic<uint64_t> rng_{0x243F6A8885A308D3ull};
};

}  // namespace leaper

#endif  // LEAPER_COLLECTOR_H_
