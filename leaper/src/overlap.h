// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Overlap check (paper Algorithm 3): intersect predicted-hot key ranges with
// block boundaries. The paper picks binary search or sort-merge depending on
// when block boundaries become known, and uses a hybrid because m (blocks) and
// n (hot ranges) vary per compaction. Same here: O(n log m) when n << m,
// O(n+m) otherwise.
//
// Both inputs must be sorted by range id. That is guaranteed because the key
// to range id mapping is order-preserving -- which is why RangeMapper requires
// monotonicity.

#ifndef LEAPER_OVERLAP_H_
#define LEAPER_OVERLAP_H_

#include <vector>

#include "leaper/leaper.h"

namespace leaper {

struct RangeSpan {
  RangeId begin = 0;  // inclusive
  RangeId end = 0;    // inclusive
};

// Merges adjacent/overlapping hot range ids into spans; input must be sorted.
std::vector<RangeSpan> ToSpans(const std::vector<RangeId>& sorted_hot);

// Returns the indices of |blocks| that overlap any span. |blocks| must be
// sorted by first_range; |spans| sorted by begin.
std::vector<size_t> SelectOverlapping(const std::vector<BlockRef>& blocks,
                                      const std::vector<RangeSpan>& spans);

// True if this single block overlaps any span. O(log n).
bool BlockOverlaps(const BlockRef& block, const std::vector<RangeSpan>& spans);

}  // namespace leaper

#endif  // LEAPER_OVERLAP_H_
