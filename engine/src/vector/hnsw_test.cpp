#include "hnsw.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <vector>

using dse::vector::DistanceMetric;
using dse::vector::HNSWIndex;
using dse::vector::Vec;

namespace {

// Brute-force reference implementation, duplicated locally for
// verification purposes only (mirrors HNSWIndex's distance semantics).
float BruteDistance(const Vec& a, const Vec& b, DistanceMetric metric) {
  if (metric == DistanceMetric::kL2) {
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
      float d = a[i] - b[i];
      sum += d * d;
    }
    return sum;
  }
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  if (na == 0.0 || nb == 0.0) return 1.0f;
  double cos_sim = dot / (std::sqrt(na) * std::sqrt(nb));
  cos_sim = std::max(-1.0, std::min(1.0, cos_sim));
  return static_cast<float>(1.0 - cos_sim);
}

std::vector<uint32_t> BruteForceTopK(const std::vector<std::pair<uint32_t, Vec>>& dataset,
                                      const Vec& query, size_t k,
                                      DistanceMetric metric) {
  std::vector<std::pair<float, uint32_t>> scored;
  scored.reserve(dataset.size());
  for (auto& [id, vec] : dataset) {
    scored.emplace_back(BruteDistance(query, vec, metric), id);
  }
  std::sort(scored.begin(), scored.end(),
            [](auto& a, auto& b) { return a.first < b.first; });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < std::min(k, scored.size()); ++i) {
    out.push_back(scored[i].second);
  }
  return out;
}

void TestSmallClusters() {
  HNSWIndex index(4, DistanceMetric::kCosine, /*M=*/16, /*ef_construction=*/200);

  // Cluster A: near {1,0,0,0}
  index.Add(1, {1.0f, 0.0f, 0.0f, 0.0f});
  index.Add(2, {0.9f, 0.1f, 0.0f, 0.0f});
  index.Add(3, {0.95f, 0.05f, 0.05f, 0.0f});

  // Cluster B: near {0,1,0,0}
  index.Add(4, {0.0f, 1.0f, 0.0f, 0.0f});
  index.Add(5, {0.1f, 0.9f, 0.0f, 0.0f});
  index.Add(6, {0.05f, 0.95f, 0.05f, 0.0f});

  assert(index.Size() == 6);
  assert(index.Dim() == 4);

  auto results_a = index.Search({1.0f, 0.0f, 0.0f, 0.0f}, 1, 50);
  assert(!results_a.empty());
  assert(results_a[0].first == 1);

  auto results_b = index.Search({0.0f, 1.0f, 0.0f, 0.0f}, 3, 50);
  assert(results_b.size() == 3);
  std::set<uint32_t> top3_b{results_b[0].first, results_b[1].first,
                             results_b[2].first};
  assert(top3_b.count(4) && top3_b.count(5) && top3_b.count(6));

  std::printf("TestSmallClusters passed\n");
}

void TestDimMismatchThrows() {
  HNSWIndex index(4, DistanceMetric::kCosine);
  bool threw = false;
  try {
    index.Add(1, {1.0f, 2.0f, 3.0f});  // wrong size
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
  std::printf("TestDimMismatchThrows passed\n");
}

std::vector<std::pair<uint32_t, Vec>> MakeRandomDataset(size_t n, size_t dim,
                                                          uint32_t seed) {
  std::mt19937 gen(seed);
  std::normal_distribution<float> dist(0.0f, 1.0f);
  std::vector<std::pair<uint32_t, Vec>> dataset;
  dataset.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    Vec v(dim);
    for (size_t d = 0; d < dim; ++d) v[d] = dist(gen);
    dataset.emplace_back(static_cast<uint32_t>(i), std::move(v));
  }
  return dataset;
}

void TestLargeDatasetSortedAndSize(DistanceMetric metric) {
  const size_t dim = 32;
  const size_t n = 1500;
  auto dataset = MakeRandomDataset(n, dim, /*seed=*/1234);

  HNSWIndex index(dim, metric, /*M=*/16, /*ef_construction=*/200);
  for (auto& [id, vec] : dataset) index.Add(id, vec);

  assert(index.Size() == n);

  std::mt19937 qgen(999);
  std::normal_distribution<float> dist(0.0f, 1.0f);

  const size_t k = 10;
  const int num_queries = 20;
  double total_overlap = 0.0;

  for (int q = 0; q < num_queries; ++q) {
    Vec query(dim);
    for (size_t d = 0; d < dim; ++d) query[d] = dist(qgen);

    auto results = index.Search(query, k, /*ef_search=*/200);

    // (a) exactly k results (dataset has more than k points)
    assert(results.size() == k);

    // (b) sorted best-first: similarity non-increasing
    for (size_t i = 1; i < results.size(); ++i) {
      assert(results[i].second <= results[i - 1].second + 1e-6f);
    }

    // (c) recall sanity check vs brute force
    auto brute = BruteForceTopK(dataset, query, k, metric);
    std::set<uint32_t> brute_set(brute.begin(), brute.end());
    size_t overlap = 0;
    for (auto& [id, sim] : results) {
      if (brute_set.count(id)) ++overlap;
    }
    total_overlap += static_cast<double>(overlap) / k;
  }

  double avg_overlap = total_overlap / num_queries;
  std::printf("Average top-%zu overlap with brute force (metric=%d): %.2f\n",
              k, static_cast<int>(metric), avg_overlap);
  assert(avg_overlap >= 0.7);

  std::printf("TestLargeDatasetSortedAndSize(metric=%d) passed\n",
              static_cast<int>(metric));
}

void TestFewerThanKPoints() {
  HNSWIndex index(4, DistanceMetric::kCosine);
  index.Add(1, {1.0f, 0.0f, 0.0f, 0.0f});
  index.Add(2, {0.0f, 1.0f, 0.0f, 0.0f});

  auto results = index.Search({1.0f, 0.0f, 0.0f, 0.0f}, 10, 50);
  assert(results.size() == 2);
  std::printf("TestFewerThanKPoints passed\n");
}

void TestMetricsDifferentRankings() {
  // Construct a case where cosine vs L2 can plausibly rank differently:
  // vector close in direction but far in magnitude vs vector further in
  // direction but closer in magnitude.
  HNSWIndex cosine_index(3, DistanceMetric::kCosine);
  HNSWIndex l2_index(3, DistanceMetric::kL2);

  cosine_index.Add(1, {10.0f, 0.0f, 0.0f});   // same direction as query, far in L2
  cosine_index.Add(2, {1.0f, 0.9f, 0.0f});    // close in L2, different direction
  l2_index.Add(1, {10.0f, 0.0f, 0.0f});
  l2_index.Add(2, {1.0f, 0.9f, 0.0f});

  Vec query{1.0f, 0.0f, 0.0f};

  auto cos_results = cosine_index.Search(query, 2, 50);
  auto l2_results = l2_index.Search(query, 2, 50);

  assert(cos_results.size() == 2);
  assert(l2_results.size() == 2);

  // Cosine should rank id 1 first (perfectly aligned direction).
  assert(cos_results[0].first == 1);
  // L2 should rank id 2 first (closer in raw distance).
  assert(l2_results[0].first == 2);

  std::printf("TestMetricsDifferentRankings passed\n");
}

}  // namespace

int main() {
  TestSmallClusters();
  TestDimMismatchThrows();
  TestFewerThanKPoints();
  TestMetricsDifferentRankings();
  TestLargeDatasetSortedAndSize(DistanceMetric::kCosine);
  TestLargeDatasetSortedAndSize(DistanceMetric::kL2);

  std::printf("hnsw tests passed\n");
  return 0;
}
