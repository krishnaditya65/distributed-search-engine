// shard.h — the top-level integration point of the engine: one Shard owns
// one WAL, one mutable in-memory "memtable" (an InvertedIndex plus doc
// store), zero-or-more immutable on-disk Segments, and one in-memory HNSW
// vector index. It implements document CRUD, hybrid keyword+vector search
// with filters and BM25 scoring, per-shard aggregations, and LSM
// flush/merge administration. This is the class the gRPC service layer
// (rpc/engine_service.*) is a thin wrapper around.
//
// Concurrency: a single mutex guards all mutable state. This is intentionally
// simple (not lock-free / not read-write-split) for an educational-scope
// implementation — see DOCUMENTATION.md for the concurrency-model discussion
// and what a production system would do differently (e.g. RCU-style
// snapshot-and-swap of the segment list so readers never block on writers).
//
// Document identity & updates: every physically-indexed occurrence of a
// document gets a fresh, monotonically increasing `internal_id` (uint32).
// "Updating" a document (re-indexing the same string doc_id) is implemented
// as delete-old-internal-id + index-under-a-new-internal-id — ids are never
// reused. This is what lets InvertedIndex and Segment stay append-only /
// immutable (see index/inverted_index.h and segment/segment.h).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aggregation/aggregator.h"
#include "analysis/analyzer.h"
#include "bitmap/roaring_bitmap.h"
#include "document/document.h"
#include "index/inverted_index.h"
#include "scoring/bm25.h"
#include "segment/segment.h"
#include "vector/hnsw.h"
#include "wal/wal.h"

namespace dse::shard {

struct IndexResult {
  bool success = false;
  std::string error;
  int64_t version = 0;
  std::string doc_id;
};

struct GetResult {
  bool found = false;
  std::string json;
};

struct DeleteResult {
  bool found = false;
};

struct BulkOpInput {
  bool is_delete = false;
  std::string doc_id;   // required for delete; optional (auto-generated if empty) for index
  std::string json;      // required for index
};

struct BulkOpResult {
  bool success = false;
  std::string error;
  std::string doc_id;
};

struct TermFilterQ {
  std::string field;
  std::string value;
};

struct RangeFilterQ {
  std::string field;
  bool has_gte = false;
  double gte = 0;
  bool has_lte = false;
  double lte = 0;
};

struct VectorQueryQ {
  std::string field;
  std::vector<float> embedding;
  int k = 10;
};

struct AggregationQ {
  std::string name;
  std::string type;   // "terms" | "stats" | "histogram"
  std::string field;
  int size = 10;
  double interval = 1.0;
};

struct SearchParamsQ {
  std::string query_text;
  std::vector<std::string> fields;
  std::vector<TermFilterQ> term_filters;
  std::vector<RangeFilterQ> range_filters;
  bool has_vector = false;
  VectorQueryQ vector;
  double vector_weight = 0.5;
  std::vector<AggregationQ> aggregations;
  int size = 10;
  int from = 0;
};

struct SearchHitR {
  std::string doc_id;
  double score = 0;
  std::string json;
};

struct SearchResultR {
  bool success = true;
  std::string error;
  std::vector<SearchHitR> hits;
  int64_t total_hits = 0;
  std::vector<dse::aggregation::AggResult> aggregations;
  int64_t took_micros = 0;
};

struct ShardStatusR {
  bool exists = true;
  int64_t doc_count = 0;
  int64_t deleted_count = 0;
  int32_t segment_count = 0;
  int64_t wal_size_bytes = 0;
  int64_t memtable_doc_count = 0;
};

struct FlushResultR {
  bool success = false;
  std::string error;
  int32_t segment_id = -1;
};

struct MergeResultR {
  bool success = false;
  std::string error;
  int32_t segments_merged = 0;
  int32_t result_segment_id = -1;
};

class Shard {
 public:
  // `data_dir` is created if it doesn't exist. If it already contains
  // segments/a WAL from a previous run (crash or restart), they are loaded
  // and replayed automatically (this IS the crash-recovery path).
  Shard(std::string shard_id, std::string data_dir, std::vector<dse::document::FieldMapping> mappings);
  ~Shard();

  Shard(const Shard&) = delete;
  Shard& operator=(const Shard&) = delete;

  IndexResult Index(const std::string& doc_id_in, const std::string& json);
  GetResult Get(const std::string& doc_id);
  DeleteResult Delete(const std::string& doc_id);
  std::vector<BulkOpResult> Bulk(const std::vector<BulkOpInput>& ops);
  SearchResultR Search(const SearchParamsQ& params);
  FlushResultR Flush();
  MergeResultR Merge();
  ShardStatusR Status();

  const std::string& ID() const { return shard_id_; }

 private:
  // --- setup / recovery ---
  void LoadExistingSegments();
  void LoadTombstones();
  void PersistTombstones();
  void ReplayWAL();
  void RebuildVectorIndexFromCurrentState();

  // --- indexing internals (each takes the lock already held by caller unless noted) ---
  IndexResult ApplyIndexLocked(const std::string& doc_id_in, const std::string& json, bool from_wal_or_load);
  bool ApplyDeleteLocked(const std::string& doc_id, bool from_wal_or_load);
  std::optional<uint32_t> ResolveInternalIdLocked(const std::string& doc_id) const;
  std::optional<std::string> GetSourceLocked(uint32_t internal_id) const;
  bool IsLiveLocked(uint32_t internal_id) const;

  // --- search internals ---
  struct TermMatchResult {
    dse::bitmap::RoaringBitmap doc_ids;               // live docs containing the term
    std::unordered_map<uint32_t, uint32_t> freqs;      // doc_id -> term frequency
  };
  dse::bitmap::RoaringBitmap MatchAllLiveLocked() const;
  dse::bitmap::RoaringBitmap TermMatchLocked(const std::string& field, const std::string& term) const;
  TermMatchResult TermMatchWithFreqsLocked(const std::string& field, const std::string& term) const;
  uint32_t DocFieldLengthLocked(uint32_t doc_id, const std::string& field) const;
  double AvgFieldLengthLocked(const std::string& field) const;
  uint64_t TotalLiveDocsLocked() const;
  std::optional<std::string> ResolveDocIdStringLocked(uint32_t internal_id) const;

  // --- misc ---
  std::string NextSegmentPath();
  const dse::document::FieldMapping* FindMapping(const std::string& field) const;

  mutable std::mutex mu_;

  std::string shard_id_;
  std::string data_dir_;
  std::vector<dse::document::FieldMapping> mappings_;

  dse::analysis::Analyzer analyzer_;
  dse::scoring::BM25Scorer bm25_;
  std::unique_ptr<dse::wal::WriteAheadLog> wal_;

  // memtable: mutable, in-RAM, not-yet-flushed documents
  dse::index::InvertedIndex memtable_index_;
  struct MemDoc {
    uint32_t internal_id;
    std::string doc_id;
    std::string source_json;
  };
  std::vector<MemDoc> memtable_docs_;                    // append-only list, order = insertion order
  std::unordered_map<uint32_t, size_t> memtable_id_to_slot_;  // internal_id -> index into memtable_docs_

  // immutable, on-disk, mmap'd segments (oldest first)
  std::vector<std::unique_ptr<dse::segment::Segment>> segments_;
  int next_segment_id_ = 0;

  // shard-wide id resolution and tombstones, spanning memtable + all segments
  std::unordered_map<std::string, uint32_t> doc_id_to_internal_;  // latest internal id per string doc_id
  dse::bitmap::RoaringBitmap deleted_ids_;                          // tombstones (internal ids)
  uint32_t next_internal_id_ = 1;

  // single, un-segmented, in-memory vector index per vector field (see
  // DOCUMENTATION.md "Vector index persistence" for why this is not
  // segmented/mmap'd like the keyword index — it is rebuilt at startup by
  // replaying the WAL and scanning stored segment sources).
  std::unordered_map<std::string, std::unique_ptr<dse::vector::HNSWIndex>> vector_indexes_;

  int64_t auto_id_counter_ = 0;
};

}  // namespace dse::shard
