#include "aggregator.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

using dse::aggregation::AggResult;
using dse::aggregation::Aggregator;
using dse::aggregation::FieldValue;
using dse::aggregation::ValueLookup;

namespace {

FieldValue StrVal(const std::string& s) {
  FieldValue v;
  v.is_number = false;
  v.str_val = s;
  return v;
}

FieldValue NumVal(double n) {
  FieldValue v;
  v.is_number = true;
  v.num_val = n;
  return v;
}

}  // namespace

void TestComputeTerms() {
  // 20 docs. brand field. Some missing.
  std::unordered_map<uint32_t, FieldValue> data;
  data[1] = StrVal("acme");
  data[2] = StrVal("acme");
  data[3] = StrVal("acme");
  data[4] = StrVal("acme");
  data[5] = StrVal("acme");
  data[6] = StrVal("globex");
  data[7] = StrVal("globex");
  data[8] = StrVal("globex");
  data[9] = StrVal("initech");
  data[10] = StrVal("initech");
  data[11] = StrVal("umbrella");
  // 12..20 missing the field entirely (excluded).

  std::vector<uint32_t> doc_ids;
  for (uint32_t i = 1; i <= 20; ++i) doc_ids.push_back(i);

  ValueLookup lookup = [&data](uint32_t doc_id) -> std::optional<FieldValue> {
    auto it = data.find(doc_id);
    if (it == data.end()) return std::nullopt;
    return it->second;
  };

  // No truncation: size large enough for all distinct values.
  AggResult full = Aggregator::ComputeTerms("brand", doc_ids, lookup, 10);
  assert(full.name == "brand");
  assert(full.type == "terms");
  assert(full.buckets.size() == 4);
  assert(full.buckets[0].key == "acme" && full.buckets[0].doc_count == 5);
  assert(full.buckets[1].key == "globex" && full.buckets[1].doc_count == 3);
  assert(full.buckets[2].key == "initech" && full.buckets[2].doc_count == 2);
  assert(full.buckets[3].key == "umbrella" && full.buckets[3].doc_count == 1);

  // Truncation to top 2.
  AggResult top2 = Aggregator::ComputeTerms("brand", doc_ids, lookup, 2);
  assert(top2.buckets.size() == 2);
  assert(top2.buckets[0].key == "acme" && top2.buckets[0].doc_count == 5);
  assert(top2.buckets[1].key == "globex" && top2.buckets[1].doc_count == 3);

  // Tie-break: initech (2) and globex (3) aren't tied, so add a tie case.
  std::unordered_map<uint32_t, FieldValue> tie_data;
  tie_data[1] = StrVal("b");
  tie_data[2] = StrVal("a");
  tie_data[3] = StrVal("c");
  std::vector<uint32_t> tie_ids = {1, 2, 3};
  ValueLookup tie_lookup = [&tie_data](uint32_t doc_id) -> std::optional<FieldValue> {
    auto it = tie_data.find(doc_id);
    if (it == tie_data.end()) return std::nullopt;
    return it->second;
  };
  AggResult tie_result = Aggregator::ComputeTerms("x", tie_ids, tie_lookup, 10);
  // all counts == 1, so ties broken by key ascending: a, b, c
  assert(tie_result.buckets.size() == 3);
  assert(tie_result.buckets[0].key == "a");
  assert(tie_result.buckets[1].key == "b");
  assert(tie_result.buckets[2].key == "c");

  std::printf("TestComputeTerms passed\n");
}

void TestComputeStats() {
  std::unordered_map<uint32_t, FieldValue> data;
  data[1] = NumVal(10.0);
  data[2] = NumVal(20.0);
  data[3] = NumVal(30.0);
  data[4] = NumVal(40.0);
  // 5, 6 missing the field entirely.

  std::vector<uint32_t> doc_ids = {1, 2, 3, 4, 5, 6};
  ValueLookup lookup = [&data](uint32_t doc_id) -> std::optional<FieldValue> {
    auto it = data.find(doc_id);
    if (it == data.end()) return std::nullopt;
    return it->second;
  };

  AggResult result = Aggregator::ComputeStats("price", doc_ids, lookup);
  assert(result.name == "price");
  assert(result.type == "stats");
  assert(result.stats.count == 4);
  assert(std::fabs(result.stats.sum - 100.0) < 1e-9);
  assert(std::fabs(result.stats.min - 10.0) < 1e-9);
  assert(std::fabs(result.stats.max - 40.0) < 1e-9);
  assert(std::fabs(result.stats.avg - 25.0) < 1e-9);

  std::printf("TestComputeStats passed\n");
}

void TestComputeStatsEmpty() {
  std::vector<uint32_t> doc_ids = {1, 2, 3};
  ValueLookup lookup = [](uint32_t) -> std::optional<FieldValue> {
    return std::nullopt;
  };

  AggResult result = Aggregator::ComputeStats("price", doc_ids, lookup);
  assert(result.stats.count == 0);
  assert(result.stats.sum == 0.0);
  assert(result.stats.min == 0.0);
  assert(result.stats.max == 0.0);
  assert(result.stats.avg == 0.0);
  assert(!std::isnan(result.stats.avg));

  std::printf("TestComputeStatsEmpty passed\n");
}

void TestComputeHistogram() {
  // interval = 10. Values chosen to land in buckets 0, 5(??) -- actually
  // bucket keys must be multiples of interval: 0, 10, 20.
  // We want bucket keys "0", "5", "10", "20" as the prompt suggests using
  // interval 5 for that specific case. Let's use interval=5.
  std::unordered_map<uint32_t, FieldValue> data;
  data[1] = NumVal(1.0);   // bucket 0
  data[2] = NumVal(3.0);   // bucket 0
  data[3] = NumVal(7.0);   // bucket 5
  data[4] = NumVal(12.0);  // bucket 10
  data[5] = NumVal(14.0);  // bucket 10
  data[6] = NumVal(14.5);  // bucket 10
  data[7] = NumVal(22.0);  // bucket 20
  data[8] = StrVal("not a number");  // skipped
  // doc 9 missing entirely -> skipped

  std::vector<uint32_t> doc_ids = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  ValueLookup lookup = [&data](uint32_t doc_id) -> std::optional<FieldValue> {
    auto it = data.find(doc_id);
    if (it == data.end()) return std::nullopt;
    return it->second;
  };

  AggResult result = Aggregator::ComputeHistogram("price", doc_ids, lookup, 5.0);
  assert(result.name == "price");
  assert(result.type == "histogram");
  assert(result.buckets.size() == 4);

  // Numeric ascending order: 0, 5, 10, 20 (lexicographic would put 10, 20
  // before 5, which would be WRONG).
  assert(result.buckets[0].key == "0" && result.buckets[0].doc_count == 2);
  assert(result.buckets[1].key == "5" && result.buckets[1].doc_count == 1);
  assert(result.buckets[2].key == "10" && result.buckets[2].doc_count == 3);
  assert(result.buckets[3].key == "20" && result.buckets[3].doc_count == 1);

  std::printf("TestComputeHistogram passed\n");
}

void TestComputeHistogramInvalidInterval() {
  std::unordered_map<uint32_t, FieldValue> data;
  data[1] = NumVal(5.0);
  data[2] = NumVal(10.0);

  std::vector<uint32_t> doc_ids = {1, 2};
  ValueLookup lookup = [&data](uint32_t doc_id) -> std::optional<FieldValue> {
    auto it = data.find(doc_id);
    if (it == data.end()) return std::nullopt;
    return it->second;
  };

  AggResult zero_interval = Aggregator::ComputeHistogram("price", doc_ids, lookup, 0.0);
  assert(zero_interval.buckets.empty());

  AggResult negative_interval = Aggregator::ComputeHistogram("price", doc_ids, lookup, -5.0);
  assert(negative_interval.buckets.empty());

  std::printf("TestComputeHistogramInvalidInterval passed\n");
}

int main() {
  TestComputeTerms();
  TestComputeStats();
  TestComputeStatsEmpty();
  TestComputeHistogram();
  TestComputeHistogramInvalidInterval();

  std::printf("aggregation tests passed\n");
  return 0;
}
