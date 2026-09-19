// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).

#include "overlap.h"

#include <algorithm>

namespace leaper {

std::vector<RangeSpan> ToSpans(const std::vector<RangeId>& sorted_hot) {
  std::vector<RangeSpan> spans;
  for (RangeId r : sorted_hot) {
    if (!spans.empty() && r <= spans.back().end + 1) {
      spans.back().end = std::max(spans.back().end, r);
    } else {
      spans.push_back(RangeSpan{r, r});
    }
  }
  return spans;
}

bool BlockOverlaps(const BlockRef& block, const std::vector<RangeSpan>& spans) {
  // First span whose end >= block.first_range.
  size_t lo = 0, hi = spans.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (spans[mid].end < block.first_range) lo = mid + 1;
    else hi = mid;
  }
  return lo < spans.size() && spans[lo].begin <= block.last_range;
}

std::vector<size_t> SelectOverlapping(const std::vector<BlockRef>& blocks,
                                      const std::vector<RangeSpan>& spans) {
  std::vector<size_t> out;
  if (blocks.empty() || spans.empty()) return out;

  const size_t m = blocks.size(), n = spans.size();
  // Cost model from the paper: binary search is O(n log m), sort-merge O(n+m).
  double log_m = 1.0;
  for (size_t x = m; x > 1; x >>= 1) log_m += 1.0;
  if (static_cast<double>(n) * log_m < static_cast<double>(n + m)) {
    for (size_t i = 0; i < m; ++i) {
      if (BlockOverlaps(blocks[i], spans)) out.push_back(i);
    }
    return out;
  }

  // Blocks are sorted by first_range but come from several SSTs, so they can
  // overlap or nest ([0,10] then [1,2]); a classic two-pointer merge that
  // advances past a block once it has been matched would skip the nested
  // one. Instead the span cursor only ever moves forward: a span that ends
  // before this block's first_range also ends before every later block's,
  // so it can be discarded for good. Each block is then tested against the
  // first span that could still reach it. O(m + n).
  size_t j = 0;
  for (size_t i = 0; i < m; ++i) {
    while (j < n && spans[j].end < blocks[i].first_range) ++j;
    if (j == n) break;
    if (spans[j].begin <= blocks[i].last_range) out.push_back(i);
  }
  return out;
}

}  // namespace leaper
