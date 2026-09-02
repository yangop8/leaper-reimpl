// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Per-component cost of the collector, in the paper's Table 5 terms.
//
// End-to-end QPS on a laptop cannot resolve a 1% overhead: five repeats of an
// unthrottled run gave stock-vs-Leaper ratios from -10.8% to +27.5%, with an
// 18% standard deviation on stock alone. The paper's Table 5 reports the
// collector as "<1 us/query" and inference as milliseconds per compaction --
// per-component figures that do not depend on the machine's mood. This
// measures the collector's Record() the same way: nanoseconds per call, single
// thread and contended.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "collector.h"

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::atoi(argv[1]) : 4;
  const uint64_t per_thread = 5'000'000;
  const uint64_t ranges = 1024;

  for (double sample : {1.0, 0.01}) {
    leaper::Collector c(ranges, 6, 1.0, sample);
    std::atomic<uint64_t> total_ns{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
      ts.emplace_back([&, t] {
        std::mt19937_64 rng(t + 1);
        // Skewed range ids so that hot cells are contended, as in a real workload.
        std::geometric_distribution<int> geo(0.05);
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < per_thread; ++i) {
          const uint64_t r = static_cast<uint64_t>(geo(rng)) % ranges;
          c.Record(r, (i & 3) == 0, 1'000'000 + i / 50'000);  // ~100 slots
        }
        const auto dt = std::chrono::steady_clock::now() - t0;
        total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
      });
    }
    for (auto& t : ts) t.join();
    const double ns_per_call = static_cast<double>(total_ns.load()) / (per_thread * threads);
    std::printf("collector Record(): sample=%.2f threads=%d  %.1f ns/call  "
                "(%.1f M calls/s/thread)\n",
                sample, threads, ns_per_call, 1000.0 / ns_per_call);
  }
  return 0;
}
