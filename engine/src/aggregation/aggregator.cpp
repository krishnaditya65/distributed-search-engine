#include "aggregator.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace dse::aggregation {

namespace {

// Formats a double using std::to_string and then trims trailing zeros (and a
// trailing decimal point, if left dangling) for a stable, readable key. This
// keeps integral values like 5.0 rendering as "5" rather than "5.000000",
// while still being a simple, mergeable string primitive for the downstream
// coordinator.
std::string FormatNumber(double value) {
  std::string formatted = std::to_string(value);
  if (formatted.find('.') != std::string::npos) {
    size_t last_non_zero = formatted.find_last_not_of('0');
    if (formatted[last_non_zero] == '.') {
      last_non_zero--;
    }
    formatted.erase(last_non_zero + 1);
  }
  return formatted;
}

std::string FieldValueToKey(const FieldValue& value) {
  if (value.is_number) {
    return FormatNumber(value.num_val);
  }
  return value.str_val;
}

}  // namespace

AggResult Aggregator::ComputeTerms(const std::string& name,
                                    const std::vector<uint32_t>& doc_ids,
                                    const ValueLookup& lookup, int size) {
  AggResult result;
  result.name = name;
  result.type = "terms";

  std::unordered_map<std::string, int64_t> counts;
  for (uint32_t doc_id : doc_ids) {
    std::optional<FieldValue> value = lookup(doc_id);
    if (!value.has_value()) {
      continue;
    }
    counts[FieldValueToKey(*value)]++;
  }

  std::vector<Bucket> buckets;
  buckets.reserve(counts.size());
  for (auto& [key, count] : counts) {
    buckets.push_back(Bucket{key, count});
  }

  std::sort(buckets.begin(), buckets.end(), [](const Bucket& a, const Bucket& b) {
    if (a.doc_count != b.doc_count) {
      return a.doc_count > b.doc_count;
    }
    return a.key < b.key;
  });

  if (size >= 0 && static_cast<size_t>(size) < buckets.size()) {
    buckets.resize(static_cast<size_t>(size));
  }

  result.buckets = std::move(buckets);
  return result;
}

AggResult Aggregator::ComputeStats(const std::string& name,
                                    const std::vector<uint32_t>& doc_ids,
                                    const ValueLookup& lookup) {
  AggResult result;
  result.name = name;
  result.type = "stats";

  StatsResult stats;
  bool has_any = false;

  for (uint32_t doc_id : doc_ids) {
    std::optional<FieldValue> value = lookup(doc_id);
    if (!value.has_value() || !value->is_number) {
      continue;
    }
    double num = value->num_val;
    if (!has_any) {
      stats.min = num;
      stats.max = num;
      has_any = true;
    } else {
      stats.min = std::min(stats.min, num);
      stats.max = std::max(stats.max, num);
    }
    stats.sum += num;
    stats.count++;
  }

  if (stats.count > 0) {
    stats.avg = stats.sum / static_cast<double>(stats.count);
  } else {
    stats.min = 0.0;
    stats.max = 0.0;
    stats.avg = 0.0;
  }

  result.stats = stats;
  return result;
}

AggResult Aggregator::ComputeHistogram(const std::string& name,
                                        const std::vector<uint32_t>& doc_ids,
                                        const ValueLookup& lookup,
                                        double interval) {
  AggResult result;
  result.name = name;
  result.type = "histogram";

  if (interval <= 0.0) {
    return result;
  }

  // Map numeric bucket key -> count, so we can sort numerically before
  // converting keys to strings for the output.
  std::map<double, int64_t> counts;
  for (uint32_t doc_id : doc_ids) {
    std::optional<FieldValue> value = lookup(doc_id);
    if (!value.has_value() || !value->is_number) {
      continue;
    }
    double bucket_key = std::floor(value->num_val / interval) * interval;
    counts[bucket_key]++;
  }

  std::vector<Bucket> buckets;
  buckets.reserve(counts.size());
  for (auto& [bucket_key, count] : counts) {
    buckets.push_back(Bucket{FormatNumber(bucket_key), count});
  }

  result.buckets = std::move(buckets);
  return result;
}

}  // namespace dse::aggregation
