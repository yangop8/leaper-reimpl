// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).

#include "collector.h"

namespace leaper {
namespace {
size_t RoundUpPow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}
}  // namespace

Collector::Collector(size_t max_ranges, int history_slots, double slot_seconds,
                     double sample_rate)
    : history_(history_slots > 0 ? history_slots : 1),
      ring_((history_slots > 0 ? history_slots : 1) + 1),
      slot_us_(static_cast<uint64_t>(slot_seconds * 1e6)),
      sample_rate_(sample_rate) {
  const size_t slots = RoundUpPow2(max_ranges > 0 ? max_ranges : 1024);
  mask_ = slots - 1;
  reads_ = std::vector<std::atomic<uint64_t>>(slots * ring_);
  writes_ = std::vector<std::atomic<uint64_t>>(slots * ring_);
  for (size_t i = 0; i < reads_.size(); ++i) {
    reads_[i].store(0, std::memory_order_relaxed);
    writes_[i].store(0, std::memory_order_relaxed);
  }
}

void Collector::Record(RangeId range, bool is_write, uint64_t now_us) {
  if (is_write) writes_seen_.fetch_add(1, std::memory_order_relaxed);
  else reads_seen_.fetch_add(1, std::memory_order_relaxed);

  const uint64_t epoch = SlotOf(now_us);
  const size_t cell = Cell(Slot(range), epoch);
  std::atomic<uint64_t>& c = is_write ? writes_[cell] : reads_[cell];

  uint64_t v = c.load(std::memory_order_relaxed);
  if ((v >> 32) != epoch) {
    // First access of this range in this slot. The paper always records it --
    // lazy initialisation plus double-checked locking -- and only samples the
    // accesses after it. That is what preserves the 0/1 property the binary
    // classifier is built on: a range accessed once would otherwise vanish
    // with probability 1-P and its label would flip. The estimator that goes
    // with it is N = (S-1)/P + 1, applied in History().
    if (c.compare_exchange_strong(v, (epoch << 32) | 1,
                                  std::memory_order_relaxed,
                                  std::memory_order_relaxed)) {
      sampled_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    v = c.load(std::memory_order_relaxed);
    if ((v >> 32) != epoch) return;  // lost the race twice; drop it
  }

  if (sample_rate_ < 1.0) {
    // xorshift; cheaper than a thread_local PRNG and good enough for sampling.
    uint64_t x = rng_.load(std::memory_order_relaxed);
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_.store(x, std::memory_order_relaxed);
    if ((x >> 11) * (1.0 / 9007199254740992.0) >= sample_rate_) return;
  }
  sampled_.fetch_add(1, std::memory_order_relaxed);
  c.fetch_add(1, std::memory_order_relaxed);
}

void Collector::History(RangeId range, uint64_t now_us, float* reads,
                        float* writes) const {
  const uint64_t epoch = SlotOf(now_us);
  const size_t idx = Slot(range);
  // Oldest first: slot epoch-history .. epoch-1.
  for (int i = 0; i < history_; ++i) {
    const uint64_t e = epoch - static_cast<uint64_t>(history_ - i);
    const size_t cell = Cell(idx, e);
    reads[i] = Estimate(CountOf(reads_[cell].load(std::memory_order_relaxed), e));
    writes[i] = Estimate(CountOf(writes_[cell].load(std::memory_order_relaxed), e));
  }
}

}  // namespace leaper
