// inverted_index.h — the core data structure: maps (field, term) -> the set
// of document ids containing that term in that field, plus per-doc term
// frequency (needed for BM25) and per-field document-length statistics
// (needed for BM25's length normalization).
//
// Design decisions (see DOCUMENTATION.md for the full rationale):
//   - Documents are NEVER updated in place. An "update" at the shard level is
//     always a delete-of-old-internal-id + index-under-a-new-internal-id.
//     This means InvertedIndex itself never needs to support removing a
//     term/doc pairing — deletions are handled entirely by the shard's
//     global tombstone set, checked at query time and physically reclaimed
//     during segment merge. This mirrors how Lucene (and this engine's
//     immutable-segment design) handles mutability.
//   - This structure is used as the shard's *mutable memtable* index (terms
//     accumulate in RAM until the next Flush turns them into an immutable
//     on-disk Segment, which has its own, separately-materialized copy of
//     equivalent postings read back from disk).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "bitmap/roaring_bitmap.h"

namespace dse::index {

// Postings for one (field, term) pair: which documents contain the term, and
// how many times. `term_freqs[i]` corresponds to the i-th id in
// `doc_ids.ToVector()` (ascending order) — callers must not assume any other
// alignment.
struct Postings {
  dse::bitmap::RoaringBitmap doc_ids;
  std::vector<uint32_t> term_freqs;
};

class InvertedIndex {
 public:
  InvertedIndex() = default;

  // Indexes an already-analyzed term list (duplicates preserved — that's how
  // term frequency is derived) for `doc_id` under `field`. Must be called at
  // most once per (doc_id, field) pair (the shard-level "update = new
  // internal id" policy guarantees this).
  void IndexField(uint32_t doc_id, const std::string& field,
                   const std::vector<std::string>& terms);

  // Returns nullptr if the (field, term) pair has never been indexed.
  const Postings* GetPostings(const std::string& field, const std::string& term) const;

  uint32_t DocFieldLength(uint32_t doc_id, const std::string& field) const;
  double AvgFieldLength(const std::string& field) const;
  uint64_t DocFreq(const std::string& field, const std::string& term) const;

  // Number of documents that have a value indexed under `field` (the
  // denominator behind AvgFieldLength; exposed so callers combining stats
  // across multiple InvertedIndex/Segment sources can compute a correctly
  // weighted average).
  uint64_t FieldDocCount(const std::string& field) const;

  // All fields that have ever had at least one document indexed under them.
  std::vector<std::string> Fields() const;

  // Read-only access to the full nested postings map, for SegmentWriter to
  // snapshot when flushing this memtable to an immutable on-disk segment.
  const std::unordered_map<std::string, std::unordered_map<std::string, Postings>>&
  AllPostings() const {
    return postings_;
  }
  const std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>>&
  AllDocFieldLengths() const {
    return doc_field_lengths_;
  }

 private:
  // field -> term -> postings
  std::unordered_map<std::string, std::unordered_map<std::string, Postings>> postings_;
  // field -> doc_id -> length (token count for that field in that doc)
  std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>> doc_field_lengths_;
  // field -> sum of lengths across docs that have the field (for avg)
  std::unordered_map<std::string, uint64_t> field_total_length_;
  // field -> number of docs that have the field (for avg denominator)
  std::unordered_map<std::string, uint64_t> field_doc_count_;
};

}  // namespace dse::index
