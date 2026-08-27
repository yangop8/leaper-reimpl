// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).

#include "gbdt.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace leaper {
namespace {

bool Split(const std::string& line, std::string* key, std::string* value) {
  const size_t eq = line.find('=');
  if (eq == std::string::npos) return false;
  *key = line.substr(0, eq);
  *value = line.substr(eq + 1);
  return true;
}

template <typename T, typename Conv>
std::vector<T> ParseList(const std::string& v, Conv conv) {
  std::vector<T> out;
  std::istringstream in(v);
  std::string tok;
  while (in >> tok) out.push_back(conv(tok));
  return out;
}

std::vector<int> ParseInts(const std::string& v) {
  return ParseList<int>(v, [](const std::string& s) { return std::atoi(s.c_str()); });
}
std::vector<double> ParseDoubles(const std::string& v) {
  return ParseList<double>(v, [](const std::string& s) { return std::atof(s.c_str()); });
}

}  // namespace

bool GbdtModel::LoadFile(const std::string& path, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    if (error) *error = "cannot open model file: " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return LoadString(ss.str(), error);
}

bool GbdtModel::LoadString(const std::string& text, std::string* error) {
  trees_.clear();
  max_feature_idx_ = -1;
  sigmoid_ = 1.0;

  std::istringstream in(text);
  std::string line, key, value;
  Tree cur;
  bool in_tree = false;

  auto flush_tree = [&]() {
    if (in_tree && !cur.leaf_value.empty()) trees_.push_back(cur);
    cur = Tree();
    in_tree = false;
  };

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (!Split(line, &key, &value)) continue;

    if (key == "Tree") {
      flush_tree();
      in_tree = true;
    } else if (key == "max_feature_idx") {
      max_feature_idx_ = std::atoi(value.c_str());
    } else if (key == "objective") {
      const size_t p = value.find("sigmoid:");
      if (p != std::string::npos) sigmoid_ = std::atof(value.c_str() + p + 8);
    } else if (in_tree) {
      if (key == "split_feature") cur.split_feature = ParseInts(value);
      else if (key == "threshold") cur.threshold = ParseDoubles(value);
      else if (key == "decision_type") cur.decision_type = ParseInts(value);
      else if (key == "left_child") cur.left_child = ParseInts(value);
      else if (key == "right_child") cur.right_child = ParseInts(value);
      else if (key == "leaf_value") cur.leaf_value = ParseDoubles(value);
    }
  }
  flush_tree();

  for (const Tree& t : trees_) {
    if (t.split_feature.size() != t.threshold.size() ||
        t.split_feature.size() != t.left_child.size() ||
        t.split_feature.size() != t.right_child.size()) {
      if (error) *error = "malformed tree: node arrays disagree in length";
      trees_.clear();
      return false;
    }
    // A categorical split needs a different traversal than the numerical one
    // implemented here. Refuse rather than score it wrong: the offline trainer
    // is configured to produce numerical splits only.
    for (size_t i = 0; i < t.decision_type.size(); ++i) {
      if (t.decision_type[i] & 1) {
        if (error) *error = "model contains categorical splits, which are not supported";
        trees_.clear();
        return false;
      }
    }
  }
  if (trees_.empty()) {
    if (error) *error = "no trees found in model";
    return false;
  }
  return true;
}

double GbdtModel::RawScore(const float* features, int n) const {
  double sum = 0.0;
  for (const Tree& t : trees_) {
    if (t.leaf_value.empty()) continue;
    if (t.split_feature.empty()) {  // single-leaf tree
      sum += t.leaf_value[0];
      continue;
    }
    int node = 0;
    while (node >= 0) {
      const int f = t.split_feature[node];
      const double x = (f >= 0 && f < n) ? static_cast<double>(features[f]) : 0.0;
      // LightGBM sends <= threshold to the left child.
      node = (x <= t.threshold[node]) ? t.left_child[node] : t.right_child[node];
    }
    const int leaf = -node - 1;
    if (leaf >= 0 && leaf < static_cast<int>(t.leaf_value.size())) {
      sum += t.leaf_value[leaf];
    }
  }
  return sum;
}

double GbdtModel::Predict(const float* features, int n) const {
  return 1.0 / (1.0 + std::exp(-sigmoid_ * RawScore(features, n)));
}

}  // namespace leaper
