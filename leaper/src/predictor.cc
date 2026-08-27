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
                           std::vector<RangeId>* out, uint64_t* inferences) const {
  const int n = feature_count();
  std::vector<float> f(n, 0.0f);
  const int last = static_cast<int>(models_.size());
  step_lo = std::max(1, step_lo);
  step_hi = std::min(last, step_hi);
  if (step_hi < step_lo) return;

  for (RangeId r : candidates) {
    BuildFeatures(collector, r, now_us, f.data());
    bool hot = false;
    for (int s = step_lo; s <= step_hi && !hot; ++s) {
      if (models_[s - 1].Predict(f.data(), n) >= threshold) hot = true;
      if (inferences) ++*inferences;
    }
    if (hot) out->push_back(r);
  }
  std::sort(out->begin(), out->end());
  out->erase(std::unique(out->begin(), out->end()), out->end());
}

}  // namespace leaper
