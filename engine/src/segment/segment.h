// segment.h — immutable, on-disk, memory-mapped segments (the "S" in LSM).
//
// A Segment is written ONCE (by Shard::Flush, from the in-memory memtable, or
// by SegmentMerger::Merge, from several older segments) and never modified
// again. Deletion of a document that lives in a segment does NOT rewrite the
// segment file — it is tracked by the Shard's own global tombstone set
// (a dse::bitmap::RoaringBitmap, persisted separately in tombstones.bin) and
// only physically reclaimed the next time a merge happens. This mirrors
// Lucene's ".liv" live-docs-file approach: the segment itself stays
// immutable; "is this doc still alive" is answered by a layer above it.
//
// I/O design: the whole segment file is memory-mapped read-only via
// dse::storage::MMapFile at Open() time (so the OS page cache — not a
// buffered read() copy — serves the bytes), and its records are then parsed
// ONCE into convenient in-memory maps for fast query-time lookups. This is a
// deliberate, documented simplification versus a "true" zero-copy design
// that would traverse postings directly over the mapped bytes without ever
// materializing them in the process heap; see DOCUMENTATION.md ("Segment
// storage format") for the full tradeoff discussion. The file access itself
// is still genuinely mmap-based, which is what gives segment *opening* its
// speed (no read() syscall loop, and the OS can page in only what's touched)
// even though this implementation touches every page once during parsing.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bitmap/roaring_bitmap.h"
#include "index/inverted_index.h"
#include "storage/mmap_file.h"

namespace dse::segment {

// One document's identity + stored source, as it will be written into a
// segment file.
struct DocRecord {
  uint32_t internal_id;
  std::string doc_id;
  std::string source_json;
};

// Everything SegmentWriter (Segment::Write) needs to serialize a segment.
// Callers (Shard::Flush, SegmentMerger::Merge) build one of these from
// whatever in-memory state they have and hand it off.
struct SegmentBuildInput {
  std::vector<std::string> fields;
  std::unordered_map<std::string, double> avg_field_length;
  // field -> term -> postings (doc ids + aligned term frequencies)
  std::unordered_map<std::string, std::unordered_map<std::string, dse::index::Postings>> postings;
  // field -> doc_id -> length
  std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>> doc_field_lengths;
  std::vector<DocRecord> docs;
};

class Segment {
 public:
  // Serializes `input` to a new file at `path` (overwriting if present).
  static void Write(const std::string& path, const SegmentBuildInput& input);

  // Opens and fully parses an existing segment file (mmap'd read-only).
  static std::unique_ptr<Segment> Open(const std::string& path);

  const dse::index::Postings* GetPostings(const std::string& field, const std::string& term) const;
  uint32_t DocFieldLength(uint32_t doc_id, const std::string& field) const;
  double AvgFieldLength(const std::string& field) const;
  uint64_t DocFreq(const std::string& field, const std::string& term) const;
  uint32_t FieldDocCount(const std::string& field) const;

  // Visits every (field, term, postings) triple stored in this segment, in
  // unspecified order. Used by SegmentMerger to rebuild merged postings
  // without needing full public enumeration of the internal term dictionary.
  void ForEachPosting(
      const std::function<void(const std::string& field, const std::string& term,
                                const dse::index::Postings& postings)>& visitor) const;

  uint32_t DocCount() const { return doc_count_; }
  std::vector<std::string> Fields() const;

  // Doc source / id-resolution accessors, used for Get(), building result
  // hits, aggregation value lookups, and range-filter post-filtering.
  std::optional<std::string> GetSource(uint32_t internal_id) const;
  std::optional<uint32_t> ResolveDocID(const std::string& doc_id) const;   // string -> internal id
  std::optional<std::string> ResolveInternalID(uint32_t internal_id) const; // internal id -> string

  // All internal ids stored in this segment (used by Shard to build the
  // full live-candidate set for match-all queries and for merge input).
  std::vector<uint32_t> AllDocIDs() const;

  const std::string& Path() const { return path_; }

 private:
  Segment() = default;

  std::string path_;
  std::unique_ptr<dse::storage::MMapFile> mmap_;  // kept alive for the process lifetime of this Segment

  uint32_t doc_count_ = 0;
  std::unordered_map<std::string, double> avg_field_length_;
  std::unordered_map<std::string, std::unordered_map<std::string, dse::index::Postings>> postings_;
  std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>> doc_field_lengths_;
  std::unordered_map<uint32_t, std::string> id_to_docid_;
  std::unordered_map<std::string, uint32_t> docid_to_id_;
  std::unordered_map<uint32_t, std::string> stored_source_;
};

}  // namespace dse::segment
