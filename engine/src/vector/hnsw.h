// hnsw.h
//
// A from-scratch, simplified Hierarchical Navigable Small World (HNSW)
// approximate nearest-neighbor index over dense float vectors.
//
// This is one module ("dse::vector") of a larger distributed search engine
// and is intended to power semantic/vector search, combinable with keyword
// search for hybrid search.
//
// Design notes / simplifications vs. production HNSW implementations
// (e.g. hnswlib):
//  - Neighbor selection during construction is "keep M closest" rather than
//    the full heuristic with diversity/pruning described in the original
//    HNSW paper. This is simpler and still yields good recall in practice
//    for moderate dataset sizes, at some cost to graph quality/recall vs.
//    the full heuristic.
//  - The level generator uses a fixed-seed std::mt19937 for determinism, so
//    this module's own tests are reproducible. Production HNSW usually uses
//    a non-deterministic RNG.
//  - The base layer (layer 0) uses 2*M neighbors per node (denser base
//    layer), all other layers use M neighbors per node -- this is the
//    standard HNSW convention.
//  - Duplicate ids are NOT checked/rejected: calling Add() twice with the
//    same id simply inserts a second, independent node that happens to
//    carry the same application id. Both nodes are then eligible to be
//    returned from Search(). This keeps insertion O(1) amortized and lets
//    the caller decide how to handle "updates" (e.g. filtering duplicates
//    downstream) rather than paying for a lookup on every insert.

#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace dse::vector {

using Vec = std::vector<float>;

enum class DistanceMetric { kCosine, kL2 };

class HNSWIndex {
 public:
  // dim: fixed vector dimensionality (all Add()'d vectors must have this
  //   size).
  // M: max bidirectional links per node per layer (typical 12-48; default
  //   16). Layer 0 uses 2*M links per node.
  // ef_construction: candidate list size used while building links at
  //   insert time (higher = better graph quality, slower build; typical
  //   100-400).
  explicit HNSWIndex(size_t dim, DistanceMetric metric = DistanceMetric::kCosine,
                      size_t M = 16, size_t ef_construction = 200);

  // Inserts a vector under application-chosen id `id`. Throws
  // std::invalid_argument if vector.size() != dim. See the duplicate-id
  // note above the class for what happens with repeated ids.
  void Add(uint32_t id, const Vec& vector);

  // Approximate k-nearest-neighbor search. `ef_search` is the candidate
  // list size at query time (>= k; higher = more accurate/slower; typical
  // 50-200). Returns up to k results sorted BEST FIRST.
  //
  // The float in each pair is a SIMILARITY score where higher is always
  // more similar/relevant, regardless of configured metric:
  //   - kCosine: the actual cosine similarity in [-1, 1].
  //   - kL2:     1 / (1 + squared_l2_distance), which is in (0, 1] and
  //              decreases monotonically as distance increases, so callers
  //              never need to know which metric was configured.
  std::vector<std::pair<uint32_t, float>> Search(const Vec& query, size_t k,
                                                  size_t ef_search = 50) const;

  size_t Size() const;
  size_t Dim() const;

 private:
  struct Node {
    uint32_t id;
    Vec vector;
    // neighbors_[layer] = list of neighbor node indices (internal indices,
    // not application ids) at that layer.
    std::vector<std::vector<uint32_t>> neighbors;
  };

  // Internal distance (NOT similarity): smaller = closer, for both metrics.
  // kCosine -> 1 - cosine_similarity (so 0 == identical direction).
  // kL2     -> squared L2 distance.
  float Distance(const Vec& a, const Vec& b) const;

  // Converts an internal distance value (as produced by Distance()) into
  // the externally-reported similarity score described on Search().
  float DistanceToSimilarity(float distance) const;

  size_t RandomLevel();

  // Greedy single-best descent from `entry` down to (but not including)
  // `target_layer`, returning the closest node found at target_layer.
  uint32_t GreedyDescend(uint32_t entry, const Vec& query, size_t from_layer,
                          size_t target_layer) const;

  // Beam search at a single layer starting from `entry_points`. Returns up
  // to ef candidates, sorted best (closest) first.
  std::vector<std::pair<float, uint32_t>> SearchLayer(
      const Vec& query, const std::vector<uint32_t>& entry_points,
      size_t layer, size_t ef) const;

  void Connect(uint32_t node_idx, uint32_t neighbor_idx, size_t layer);

  size_t dim_;
  DistanceMetric metric_;
  size_t M_;
  size_t M0_;  // neighbors at layer 0 (2*M_)
  size_t ef_construction_;
  double mL_;  // level normalization factor = 1 / ln(M)
  static constexpr size_t kMaxLevel = 16;

  std::vector<Node> nodes_;
  int32_t entry_point_ = -1;  // internal index of current entry point, -1 if empty
  size_t max_level_ = 0;      // highest layer currently in use (entry point's level)

  std::mt19937 rng_;  // fixed-seed for deterministic, reproducible tests
  std::uniform_real_distribution<double> unif_{0.0, 1.0};
};

}  // namespace dse::vector
