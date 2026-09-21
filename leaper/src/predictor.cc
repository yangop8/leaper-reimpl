// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).

#include "predictor.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace leaper {

bool Predictor::Load(const std::vector<std::string>& model_paths,
                     const std::string& precursor_path, int history, int gamma,
                     std::string* error) {
  history_ = history;
  gamma_ = gamma;
  models_.clear();
  models_.resize(model_paths.size());
  for (size_t i = 0; i < model_paths.size(); ++i) {
    if (!models_[i].LoadFile(model_paths[i], error)) return false;
  }
  if (models_.empty()) {
    if (error) *error = "no models given";
    return false;
  }
  for (const GbdtModel& m : models_) {
    if (m.num_features() > feature_count()) {
      if (error) {
        std::ostringstream ss;
        ss << "model expects " << m.num_features() << " features but the "
           << "configured history/gamma produce " << feature_count()
           << "; the online feature layout must match the trainer's";
        *error = ss.str();
      }
      return false;
    }
  }

  if (!precursor_path.empty()) {
    std::ifstream in(precursor_path);
    if (!in) {
      if (error) *error = "cannot open precursor file: " + precursor_path;
      return false;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ls(line);
      RangeId target;
      if (!(ls >> target)) continue;
      std::vector<RangeId> ps;
      RangeId p;
      while (ls >> p && static_cast<int>(ps.size()) < gamma_) ps.push_back(p);
      if (!ps.empty()) precursors_[target] = std::move(ps);
    }
  }
  return true;
}

void Predictor::BuildFeatures(const Collector& collector, RangeId range,
                              uint64_t now_us, float* f) const {
  collector.History(range, now_us, f, f + history_);

  const double t = static_cast<double>(now_us) / 1e6;
  const long long secs = static_cast<long long>(t);
  f[2 * history_ + 0] = static_cast<float>((secs / 3600) % 24);
  f[2 * history_ + 1] = static_cast<float>((secs / 60) % 60);
  f[2 * history_ + 2] = static_cast<float>(secs % 60);

  float* pf = f + 2 * history_ + 3;
  for (int g = 0; g < gamma_; ++g) pf[g] = 0.0f;
  const auto it = precursors_.find(range);
  if (it != precursors_.end()) {
    std::vector<float> r(history_), w(history_);
    for (size_t g = 0; g < it->second.size() && static_cast<int>(g) < gamma_; ++g) {
      collector.History(it->second[g], now_us, r.data(), w.data());
      pf[g] = r[history_ - 1];  // precursor's rate in the previous slot
    }
  }
}

void Predictor::PredictHot(const Collector& collector,
                           const std::vector<RangeId>& candidates, int step_lo,
                           int step_hi, uint64_t now_us, double threshold,
                           std::vector<RangeId>* out, uint64_t* inferences,
                           uint64_t* memo_hits) const {
  const int n = feature_count();
  std::vector<float> f(n, 0.0f);
  const int last = static_cast<int>(models_.size());
  if (last == 0) return;
  step_lo = std::max(1, step_lo);
  // leaper.h promises that a single model stands in for every step. Clamping
  // the step range to the model count broke that: a compaction's prefetch
  // phase starts at step k1+1 >= 2, so with one model it predicted nothing.
  const bool single = (last == 1);
  if (single) step_hi = step_lo;
  else step_hi = std::min(last, step_hi);
  if (step_hi < step_lo) {
    ++clamped_calls_;
    return;
  }

  Memo* memo = nullptr;
  if (memoize_) {
    const uint64_t sec = now_us / 1000000;
    const uint64_t slot = collector.SlotOf(now_us);
    // Memos from any earlier second or slot are dead; drop them all.
    memo_.erase(std::remove_if(memo_.begin(), memo_.end(),
                               [&](const Memo& m) { return m.sec != sec || m.slot != slot; }),
                memo_.end());
    for (Memo& m : memo_) {
      if (m.lo == step_lo && m.hi == step_hi) memo = &m;
    }
    if (memo == nullptr) {
      memo_.push_back(Memo{sec, slot, step_lo, step_hi, {}});
      memo = &memo_.back();
    }
  }

  for (RangeId r : candidates) {
    if (memo != nullptr && r < memo->hot.size() && memo->hot[r] >= 0) {
      if (memo->hot[r]) out->push_back(r);
      if (memo_hits) ++*memo_hits;
      continue;
    }
    BuildFeatures(collector, r, now_us, f.data());
    bool hot = false;
    for (int s = step_lo; s <= step_hi && !hot; ++s) {
      const GbdtModel& m = single ? models_[0] : models_[s - 1];
      if (m.Predict(f.data(), n) >= threshold) hot = true;
      if (inferences) ++*inferences;
    }
    if (memo != nullptr) {
      if (r >= memo->hot.size()) memo->hot.resize(r + 1, -1);
      memo->hot[r] = hot ? 1 : 0;
    }
    if (hot) out->push_back(r);
  }
  std::sort(out->begin(), out->end());
  out->erase(std::unique(out->begin(), out->end()), out->end());
}

}  // namespace leaper
