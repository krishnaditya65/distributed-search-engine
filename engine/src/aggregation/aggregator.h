#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dse::aggregation {

struct FieldValue {
  bool is_number = false;
  std::string str_val;   // valid when is_number == false
  double num_val = 0.0;  // valid when is_number == true
};

struct Bucket {
  std::string key;
  int64_t doc_count = 0;
};

struct StatsResult {
  int64_t count = 0;
  double sum = 0.0;
  double min = 0.0;
  double max = 0.0;
  double avg = 0.0;
};

struct AggResult {
  std::string name;
  std::string type;             // "terms" | "stats" | "histogram"
  std::vector<Bucket> buckets;  // populated for "terms" and "histogram"
  StatsResult stats;            // populated for "stats"
};

// Returns the value of some field for a given document id, or std::nullopt if
// that document doesn't have the field. The caller (shard integration code,
// which you are not building) supplies this closure backed by whatever
// document-field storage it uses.
using ValueLookup = std::function<std::optional<FieldValue>(uint32_t doc_id)>;

class Aggregator {
 public:
  // Buckets by the STRING representation of each doc's field value (if
  // is_number, format via a stable numeric-to-string conversion e.g. using
  // std::to_string with trailing-zero trimming, or just std::to_string is
  // acceptable — document your choice). Counts occurrences per distinct key
  // among `doc_ids`, sorts buckets by doc_count DESCENDING, ties broken by
  // key ascending (lexicographic), truncates to the top `size` buckets.
  // Docs with no value for the field are simply excluded (not counted, not
  // bucketed under any key).
  static AggResult ComputeTerms(const std::string& name,
                                 const std::vector<uint32_t>& doc_ids,
                                 const ValueLookup& lookup, int size);

  // Computes count/sum/min/max/avg over the numeric field values among
  // `doc_ids` (only docs where lookup() returns a value with is_number==true
  // are included; non-numeric or missing values are skipped). If no docs
  // have a numeric value, return a StatsResult with count=0 and all other
  // fields 0.0 (do not divide by zero for avg).
  static AggResult ComputeStats(const std::string& name,
                                 const std::vector<uint32_t>& doc_ids,
                                 const ValueLookup& lookup);

  // Buckets numeric values into fixed-width intervals: bucket_key =
  // floor(value / interval) * interval, formatted as a string (e.g. via
  // std::to_string — trailing zeros are fine). Only numeric values are
  // considered (same skip rule as ComputeStats). Buckets are sorted by their
  // NUMERIC key ascending (not lexicographic string order — e.g. bucket "9"
  // must sort before "10"). `interval` must be > 0 (if <= 0, return an empty
  // bucket list rather than crashing/dividing-by-zero-or-negative).
  static AggResult ComputeHistogram(const std::string& name,
                                     const std::vector<uint32_t>& doc_ids,
                                     const ValueLookup& lookup,
                                     double interval);
};

}  // namespace dse::aggregation
