#include "hnsw.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace dse::vector {

HNSWIndex::HNSWIndex(size_t dim, DistanceMetric metric, size_t M,
                      size_t ef_construction)
    : dim_(dim),
      metric_(metric),
      M_(M),
      M0_(2 * M),
      ef_construction_(ef_construction),
      mL_(1.0 / std::log(static_cast<double>(M > 1 ? M : 2))),
      rng_(42) {}  // fixed seed for deterministic, reproducible behavior

size_t HNSWIndex::Size() const { return nodes_.size(); }
size_t HNSWIndex::Dim() const { return dim_; }

float HNSWIndex::Distance(const Vec& a, const Vec& b) const {
  if (metric_ == DistanceMetric::kL2) {
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
      float d = a[i] - b[i];
      sum += d * d;
    }
    return sum;
  }
  // kCosine
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  if (na == 0.0 || nb == 0.0) {
    // Zero-vector convention: similarity 0, i.e. distance 1 (max, since
    // cosine similarity is bounded in [-1, 1] and distance = 1 - sim).
    return 1.0f;
  }
  double cos_sim = dot / (std::sqrt(na) * std::sqrt(nb));
  // Clamp for numerical safety.
  cos_sim = std::max(-1.0, std::min(1.0, cos_sim));
  return static_cast<float>(1.0 - cos_sim);
}

float HNSWIndex::DistanceToSimilarity(float distance) const {
  if (metric_ == DistanceMetric::kL2) {
    return 1.0f / (1.0f + distance);
  }
  // kCosine: distance = 1 - cos_sim  =>  cos_sim = 1 - distance
  return 1.0f - distance;
}

size_t HNSWIndex::RandomLevel() {
  double rand_val = unif_(rng_);
  // Avoid log(0).
  if (rand_val < 1e-12) rand_val = 1e-12;
  size_t level = static_cast<size_t>(std::floor(-std::log(rand_val) * mL_));
  return std::min(level, kMaxLevel);
}

void HNSWIndex::Add(uint32_t id, const Vec& vector) {
  if (vector.size() != dim_) {
    throw std::invalid_argument(
        "HNSWIndex::Add: vector dimension does not match index dimension");
  }

  size_t level = RandomLevel();
  uint32_t node_idx = static_cast<uint32_t>(nodes_.size());

  Node node;
  node.id = id;
  node.vector = vector;
  node.neighbors.resize(level + 1);
  nodes_.push_back(std::move(node));

  if (entry_point_ < 0) {
    entry_point_ = static_cast<int32_t>(node_idx);
    max_level_ = level;
    return;
  }

  uint32_t cur_entry = static_cast<uint32_t>(entry_point_);

  // Phase 1: greedily descend from top layer down to level+1 (ef=1),
  // narrowing to the single closest node found at each layer.
  if (max_level_ > level) {
    cur_entry = GreedyDescend(cur_entry, vector, max_level_, level);
  }

  // Phase 2: from min(level, max_level_) down to 0, beam search with
  // ef_construction candidates and connect to the M closest.
  size_t start_layer = std::min(level, max_level_);
  std::vector<uint32_t> entry_points{cur_entry};

  for (size_t layer = start_layer + 1; layer-- > 0;) {
    auto candidates =
        SearchLayer(vector, entry_points, layer, ef_construction_);

    size_t max_links = (layer == 0) ? M0_ : M_;
    size_t num_connect = std::min(max_links, candidates.size());

    // Simple "keep M closest" neighbor selection (documented
    // simplification vs. the full HNSW heuristic).
    for (size_t i = 0; i < num_connect; ++i) {
      Connect(node_idx, candidates[i].second, layer);
    }

    // Next layer's entry points: the candidates found here.
    entry_points.clear();
    for (auto& c : candidates) entry_points.push_back(c.second);
    if (entry_points.empty()) entry_points.push_back(cur_entry);
  }

  if (level > max_level_) {
    max_level_ = level;
    entry_point_ = static_cast<int32_t>(node_idx);
  }
}

void HNSWIndex::Connect(uint32_t node_idx, uint32_t neighbor_idx,
                         size_t layer) {
  auto& a_neighbors = nodes_[node_idx].neighbors[layer];
  a_neighbors.push_back(neighbor_idx);

  // Reverse link.
  auto& neighbor_node = nodes_[neighbor_idx];
  if (neighbor_node.neighbors.size() <= layer) {
    // Neighbor doesn't have this layer (shouldn't normally happen since
    // candidates come from SearchLayer at this exact layer, but guard
    // anyway).
    neighbor_node.neighbors.resize(layer + 1);
  }
  auto& b_neighbors = neighbor_node.neighbors[layer];
  b_neighbors.push_back(node_idx);

  size_t max_links = (layer == 0) ? M0_ : M_;
  if (b_neighbors.size() > max_links) {
    // Prune neighbor's link list back down to cap by dropping its
    // currently-furthest neighbor.
    const Vec& b_vec = neighbor_node.vector;
    size_t furthest_pos = 0;
    float furthest_dist = -1.0f;
    for (size_t i = 0; i < b_neighbors.size(); ++i) {
      float d = Distance(b_vec, nodes_[b_neighbors[i]].vector);
      if (d > furthest_dist) {
        furthest_dist = d;
        furthest_pos = i;
      }
    }
    b_neighbors.erase(b_neighbors.begin() + furthest_pos);
  }
}

uint32_t HNSWIndex::GreedyDescend(uint32_t entry, const Vec& query,
                                    size_t from_layer,
                                    size_t target_layer) const {
  uint32_t current = entry;
  float current_dist = Distance(query, nodes_[current].vector);

  for (size_t layer = from_layer; layer > target_layer; --layer) {
    bool improved = true;
    while (improved) {
      improved = false;
      if (layer < nodes_[current].neighbors.size()) {
        for (uint32_t nb : nodes_[current].neighbors[layer]) {
          float d = Distance(query, nodes_[nb].vector);
          if (d < current_dist) {
            current_dist = d;
            current = nb;
            improved = true;
          }
        }
      }
    }
  }
  return current;
}

std::vector<std::pair<float, uint32_t>> HNSWIndex::SearchLayer(
    const Vec& query, const std::vector<uint32_t>& entry_points,
    size_t layer, size_t ef) const {
  // visited set
  std::unordered_set<uint32_t> visited;

  // candidates: min-heap by distance (closest first) -- to explore
  using DistNode = std::pair<float, uint32_t>;
  auto min_cmp = [](const DistNode& a, const DistNode& b) {
    return a.first > b.first;
  };
  std::priority_queue<DistNode, std::vector<DistNode>, decltype(min_cmp)>
      candidates(min_cmp);

  // result: max-heap by distance (furthest first) -- current best-ef set
  auto max_cmp = [](const DistNode& a, const DistNode& b) {
    return a.first < b.first;
  };
  std::priority_queue<DistNode, std::vector<DistNode>, decltype(max_cmp)>
      result(max_cmp);

  for (uint32_t ep : entry_points) {
    if (visited.count(ep)) continue;
    visited.insert(ep);
    float d = Distance(query, nodes_[ep].vector);
    candidates.push({d, ep});
    result.push({d, ep});
  }

  while (!candidates.empty()) {
    auto [cur_dist, cur_idx] = candidates.top();
    candidates.pop();

    if (!result.empty() && cur_dist > result.top().first &&
        result.size() >= ef) {
      break;  // no closer candidates remain
    }

    if (layer < nodes_[cur_idx].neighbors.size()) {
      for (uint32_t nb : nodes_[cur_idx].neighbors[layer]) {
        if (visited.count(nb)) continue;
        visited.insert(nb);
        float d = Distance(query, nodes_[nb].vector);
        if (result.size() < ef || d < result.top().first) {
          candidates.push({d, nb});
          result.push({d, nb});
          if (result.size() > ef) result.pop();
        }
      }
    }
  }

  std::vector<DistNode> out;
  out.reserve(result.size());
  while (!result.empty()) {
    out.push_back(result.top());
    result.pop();
  }
  std::sort(out.begin(), out.end(),
            [](const DistNode& a, const DistNode& b) {
              return a.first < b.first;
            });
  return out;
}

std::vector<std::pair<uint32_t, float>> HNSWIndex::Search(
    const Vec& query, size_t k, size_t ef_search) const {
  std::vector<std::pair<uint32_t, float>> results;
  if (nodes_.empty() || entry_point_ < 0) return results;
  if (query.size() != dim_) {
    throw std::invalid_argument(
        "HNSWIndex::Search: query dimension does not match index dimension");
  }

  uint32_t entry = static_cast<uint32_t>(entry_point_);

  // Greedy descent from top layer to layer 1 (ef=1).
  if (max_level_ > 0) {
    entry = GreedyDescend(entry, query, max_level_, 0);
  }

  size_t ef = std::max(ef_search, k);
  auto candidates = SearchLayer(query, {entry}, 0, ef);

  size_t n = std::min(k, candidates.size());
  results.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    float sim = DistanceToSimilarity(candidates[i].first);
    results.emplace_back(nodes_[candidates[i].second].id, sim);
  }
  return results;
}

}  // namespace dse::vector
