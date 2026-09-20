// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
// mixgraph_check: what the FAST'20 ZippyDB model looks like at Leaper's
// granularity. Prints the key-range shares and, for one ZippyDB-second of
// reads (4,900), the fraction of 2,000-key ranges touched -- the positive
// rate Leaper's label would have.
#include <cstdio>
#include <random>
#include <set>
#include <vector>
#include "leaper_bench/keygen.h"
int main() {
  const uint64_t n = 50000000;
  leaper_bench::Mixgraph cfg;
  leaper_bench::MixgraphChooser mg(n, cfg);
  std::vector<double> sh = mg.RangeShares();
  std::vector<double> sorted = sh; std::sort(sorted.rbegin(), sorted.rend());
  std::printf("30 key ranges of %llu keys; shares (sorted): ", (unsigned long long)mg.range_keys());
  for (int i = 0; i < 6; ++i) std::printf("%.3f ", sorted[i]);
  std::printf("... %.4f\n", sorted.back());
  std::mt19937_64 rng(1);
  for (uint64_t rs : {2000ULL, 10000ULL, 100000ULL}) {
    double touched = 0; const int secs = 20;
    for (int t = 0; t < secs; ++t) {
      std::set<uint64_t> s;
      for (int i = 0; i < 4900; ++i) s.insert(mg.Next(&rng) / rs);
      touched += static_cast<double>(s.size()) / (n / rs);
    }
    std::printf("range_size %6llu: %5.1f%% of ranges touched per ZippyDB-second\n",
                (unsigned long long)rs, 100.0 * touched / secs);
  }
  return 0;
}
