#include "shard/shard.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "segment/segment_merger.h"

namespace dse::shard {

namespace fs = std::filesystem;
using dse::document::Document;

namespace {

std::string ToLowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Formats a number as an exact-match index token. Consistent between index
// time (for "number"-typed fields) and query time (TermFilter against a
// number field), so the two round-trip to the same token string.
std::string FormatNumberToken(double v) {
  if (std::floor(v) == v && std::abs(v) < 1e15) {
    return std::to_string(static_cast<int64_t>(v));
  }
  std::ostringstream oss;
  oss.precision(10);
  oss << v;
  return oss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// construction / recovery
// ---------------------------------------------------------------------------

Shard::Shard(std::string shard_id, std::string data_dir,
             std::vector<dse::document::FieldMapping> mappings)
    : shard_id_(std::move(shard_id)), data_dir_(std::move(data_dir)), mappings_(std::move(mappings)) {
  fs::create_directories(data_dir_);

  LoadTombstones();
  LoadExistingSegments();
  RebuildVectorIndexFromCurrentState();  // backfills vector_indexes_ from segment docs

  wal_ = std::make_unique<dse::wal::WriteAheadLog>(data_dir_ + "/wal.log");
  ReplayWAL();  // replays anything written since the last flush; also
                // incrementally populates vector_indexes_ for memtable docs
}

Shard::~Shard() = default;

void Shard::LoadTombstones() {
  std::string path = data_dir_ + "/tombstones.bin";
  std::ifstream in(path, std::ios::binary);
  if (!in) return;  // no tombstones yet, fine
  std::ostringstream oss;
  oss << in.rdbuf();
  deleted_ids_ = dse::bitmap::RoaringBitmap::Deserialize(oss.str());
}

void Shard::PersistTombstones() {
  std::string path = data_dir_ + "/tombstones.bin";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  std::string bytes = deleted_ids_.Serialize();
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void Shard::LoadExistingSegments() {
  if (!fs::exists(data_dir_)) return;

  std::vector<std::pair<int, std::string>> found;  // (segment id, path)
  for (const auto& entry : fs::directory_iterator(data_dir_)) {
    if (!entry.is_regular_file()) continue;
    std::string name = entry.path().filename().string();
    if (name.rfind("segment_", 0) != 0) continue;
    if (name.size() < 4 || name.substr(name.size() - 4) != ".seg") continue;
    std::string num = name.substr(8, name.size() - 8 - 4);  // strip "segment_" prefix and ".seg" suffix
    try {
      int id = std::stoi(num);
      found.emplace_back(id, entry.path().string());
    } catch (...) {
      continue;  // not a well-formed segment filename, ignore
    }
  }
  std::sort(found.begin(), found.end());

  int max_id = -1;
  for (const auto& [id, path] : found) {
    auto seg = dse::segment::Segment::Open(path);
    for (uint32_t internal_id : seg->AllDocIDs()) {
      auto doc_id = seg->ResolveInternalID(internal_id);
      if (doc_id) doc_id_to_internal_[*doc_id] = internal_id;
      next_internal_id_ = std::max(next_internal_id_, internal_id + 1);
    }
    segments_.push_back(std::move(seg));
    max_id = std::max(max_id, id);
  }
  next_segment_id_ = max_id + 1;
}

void Shard::RebuildVectorIndexFromCurrentState() {
  for (const auto& m : mappings_) {
    if (m.type != "vector" || m.vector_dim <= 0) continue;
    auto idx = std::make_unique<dse::vector::HNSWIndex>(static_cast<size_t>(m.vector_dim));
    for (const auto& seg : segments_) {
      for (uint32_t id : seg->AllDocIDs()) {
        if (deleted_ids_.Contains(id)) continue;
        auto src = seg->GetSource(id);
        if (!src) continue;
        try {
          auto doc = Document::Parse(*src);
          auto v = doc.GetVector(m.field);
          if (v && static_cast<int>(v->size()) == m.vector_dim) idx->Add(id, *v);
        } catch (...) {
          // malformed stored source should not happen (validated at index
          // time) but never let a bad doc crash startup
        }
      }
    }
    vector_indexes_[m.field] = std::move(idx);
  }
}

void Shard::ReplayWAL() {
  auto records = wal_->ReadAll();
  for (const auto& rec : records) {
    if (rec.type == dse::wal::WalOpType::kIndex) {
      ApplyIndexLocked(rec.doc_id, rec.json, /*from_wal_or_load=*/true);
    } else {
      ApplyDeleteLocked(rec.doc_id, /*from_wal_or_load=*/true);
    }
  }
}

// ---------------------------------------------------------------------------
// indexing internals
// ---------------------------------------------------------------------------

const dse::document::FieldMapping* Shard::FindMapping(const std::string& field) const {
  for (const auto& m : mappings_) {
    if (m.field == field) return &m;
  }
  return nullptr;
}

IndexResult Shard::ApplyIndexLocked(const std::string& doc_id, const std::string& json,
                                     bool from_wal_or_load) {
  std::optional<Document> doc_opt;
  try {
    doc_opt = Document::Parse(json);
  } catch (const std::exception& e) {
    if (from_wal_or_load) return {false, "", 0, doc_id};  // skip corrupt record, don't crash recovery
    return {false, e.what(), 0, doc_id};
  }
  Document& doc = *doc_opt;

  // An "update" of an existing doc_id: tombstone the old internal id (its
  // postings/stored data stay physically in place until the next merge).
  auto existing = doc_id_to_internal_.find(doc_id);
  if (existing != doc_id_to_internal_.end()) {
    deleted_ids_.Add(existing->second);
  }

  uint32_t new_id = next_internal_id_++;
  doc_id_to_internal_[doc_id] = new_id;

  for (const auto& m : mappings_) {
    if (m.type == "text") {
      auto text = doc.GetAsIndexableString(m.field);
      if (text) {
        auto terms = analyzer_.Analyze(*text);
        if (!terms.empty()) memtable_index_.IndexField(new_id, m.field, terms);
      }
    } else if (m.type == "keyword") {
      auto keyword_val = doc.GetAsIndexableString(m.field);
      if (keyword_val) memtable_index_.IndexField(new_id, m.field, {ToLowerAscii(*keyword_val)});
    } else if (m.type == "number") {
      auto number_val = doc.GetNumber(m.field);
      if (number_val) memtable_index_.IndexField(new_id, m.field, {FormatNumberToken(*number_val)});
    } else if (m.type == "vector") {
      auto vec = doc.GetVector(m.field);
      if (vec && static_cast<int>(vec->size()) == m.vector_dim) {
        auto it = vector_indexes_.find(m.field);
        if (it == vector_indexes_.end()) {
          it = vector_indexes_.emplace(m.field, std::make_unique<dse::vector::HNSWIndex>(
                                                     static_cast<size_t>(m.vector_dim))).first;
        }
        it->second->Add(new_id, *vec);
      }
    }
  }

  memtable_docs_.push_back(MemDoc{new_id, doc_id, json});
  memtable_id_to_slot_[new_id] = memtable_docs_.size() - 1;

  return {true, "", static_cast<int64_t>(new_id), doc_id};
}

bool Shard::ApplyDeleteLocked(const std::string& doc_id, bool /*from_wal_or_load*/) {
  auto it = doc_id_to_internal_.find(doc_id);
  if (it == doc_id_to_internal_.end()) return false;
  if (deleted_ids_.Contains(it->second)) return false;  // already deleted
  deleted_ids_.Add(it->second);
  return true;
}

std::optional<uint32_t> Shard::ResolveInternalIdLocked(const std::string& doc_id) const {
  auto it = doc_id_to_internal_.find(doc_id);
  if (it == doc_id_to_internal_.end()) return std::nullopt;
  return it->second;
}

bool Shard::IsLiveLocked(uint32_t internal_id) const {
  if (deleted_ids_.Contains(internal_id)) return false;
  if (memtable_id_to_slot_.count(internal_id)) return true;
  for (const auto& seg : segments_) {
    if (seg->GetSource(internal_id)) return true;
  }
  return false;
}

std::optional<std::string> Shard::GetSourceLocked(uint32_t internal_id) const {
  auto slot_it = memtable_id_to_slot_.find(internal_id);
  if (slot_it != memtable_id_to_slot_.end()) return memtable_docs_[slot_it->second].source_json;
  for (const auto& seg : segments_) {
    auto src = seg->GetSource(internal_id);
    if (src) return src;
  }
  return std::nullopt;
}

std::optional<std::string> Shard::ResolveDocIdStringLocked(uint32_t internal_id) const {
  auto slot_it = memtable_id_to_slot_.find(internal_id);
  if (slot_it != memtable_id_to_slot_.end()) return memtable_docs_[slot_it->second].doc_id;
  for (const auto& seg : segments_) {
    auto docid = seg->ResolveInternalID(internal_id);
    if (docid) return docid;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// public CRUD
// ---------------------------------------------------------------------------

IndexResult Shard::Index(const std::string& doc_id_in, const std::string& json) {
  // Validate before touching the WAL so a malformed document never becomes
  // durable state.
  try {
    Document::Parse(json);
  } catch (const std::exception& e) {
    return {false, e.what(), 0, doc_id_in};
  }

  std::lock_guard<std::mutex> lock(mu_);
  std::string doc_id = doc_id_in;
  if (doc_id.empty()) {
    doc_id = shard_id_ + "-auto-" + std::to_string(++auto_id_counter_);
  }
  wal_->Append(dse::wal::WalRecord{dse::wal::WalOpType::kIndex, doc_id, json});
  return ApplyIndexLocked(doc_id, json, /*from_wal_or_load=*/false);
}

GetResult Shard::Get(const std::string& doc_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto id = ResolveInternalIdLocked(doc_id);
  if (!id || !IsLiveLocked(*id)) return {false, ""};
  auto src = GetSourceLocked(*id);
  return {true, src.value_or("{}")};
}

DeleteResult Shard::Delete(const std::string& doc_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto id = ResolveInternalIdLocked(doc_id);
  if (!id || !IsLiveLocked(*id)) return {false};
  wal_->Append(dse::wal::WalRecord{dse::wal::WalOpType::kDelete, doc_id, ""});
  ApplyDeleteLocked(doc_id, false);
  return {true};
}

std::vector<BulkOpResult> Shard::Bulk(const std::vector<BulkOpInput>& ops) {
  std::vector<BulkOpResult> results;
  results.reserve(ops.size());
  for (const auto& op : ops) {
    if (op.is_delete) {
      auto delete_result = Delete(op.doc_id);
      results.push_back({delete_result.found, delete_result.found ? "" : "document not found", op.doc_id});
    } else {
      auto index_result = Index(op.doc_id, op.json);
      results.push_back({index_result.success, index_result.error, index_result.doc_id});
    }
  }
  return results;
}

// ---------------------------------------------------------------------------
// search internals
// ---------------------------------------------------------------------------

dse::bitmap::RoaringBitmap Shard::MatchAllLiveLocked() const {
  dse::bitmap::RoaringBitmap out;
  for (const auto& md : memtable_docs_) {
    if (!deleted_ids_.Contains(md.internal_id)) out.Add(md.internal_id);
  }
  for (const auto& seg : segments_) {
    for (uint32_t id : seg->AllDocIDs()) {
      if (!deleted_ids_.Contains(id)) out.Add(id);
    }
  }
  return out;
}

Shard::TermMatchResult Shard::TermMatchWithFreqsLocked(const std::string& field,
                                                        const std::string& term) const {
  TermMatchResult result;
  auto add_source = [&](const dse::index::Postings* p) {
    if (!p) return;
    auto ids = p->doc_ids.ToVector();
    for (size_t i = 0; i < ids.size(); i++) {
      uint32_t id = ids[i];
      if (deleted_ids_.Contains(id)) continue;
      result.doc_ids.Add(id);
      result.freqs[id] = p->term_freqs[i];
    }
  };
  add_source(memtable_index_.GetPostings(field, term));
  for (const auto& seg : segments_) add_source(seg->GetPostings(field, term));
  return result;
}

dse::bitmap::RoaringBitmap Shard::TermMatchLocked(const std::string& field,
                                                   const std::string& term) const {
  return TermMatchWithFreqsLocked(field, term).doc_ids;
}

uint32_t Shard::DocFieldLengthLocked(uint32_t doc_id, const std::string& field) const {
  uint32_t len = memtable_index_.DocFieldLength(doc_id, field);
  if (len > 0) return len;
  for (const auto& seg : segments_) {
    len = seg->DocFieldLength(doc_id, field);
    if (len > 0) return len;
  }
  return 0;
}

double Shard::AvgFieldLengthLocked(const std::string& field) const {
  double total = memtable_index_.AvgFieldLength(field) * static_cast<double>(memtable_index_.FieldDocCount(field));
  uint64_t count = memtable_index_.FieldDocCount(field);
  for (const auto& seg : segments_) {
    total += seg->AvgFieldLength(field) * static_cast<double>(seg->FieldDocCount(field));
    count += seg->FieldDocCount(field);
  }
  return count == 0 ? 0.0 : total / static_cast<double>(count);
}

uint64_t Shard::TotalLiveDocsLocked() const {
  uint64_t live = 0;
  for (const auto& md : memtable_docs_) {
    if (!deleted_ids_.Contains(md.internal_id)) live++;
  }
  for (const auto& seg : segments_) {
    for (uint32_t id : seg->AllDocIDs()) {
      if (!deleted_ids_.Contains(id)) live++;
    }
  }
  return live;
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

SearchResultR Shard::Search(const SearchParamsQ& params) {
  auto t0 = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mu_);

  SearchResultR result;

  std::vector<std::string> fields = params.fields;
  if (fields.empty()) {
    for (const auto& m : mappings_) {
      if (m.type == "text") fields.push_back(m.field);
    }
  }

  dse::bitmap::RoaringBitmap candidates;
  std::unordered_map<uint32_t, double> bm25_scores;

  if (params.query_text.empty()) {
    candidates = MatchAllLiveLocked();
  } else {
    auto terms = analyzer_.Analyze(params.query_text);
    uint64_t N = TotalLiveDocsLocked();
    for (const auto& field : fields) {
      double avg_len = AvgFieldLengthLocked(field);
      for (const auto& term : terms) {
        auto term_match = TermMatchWithFreqsLocked(field, term);
        uint64_t df = term_match.doc_ids.Cardinality();
        if (df == 0) continue;
        for (uint32_t id : term_match.doc_ids.ToVector()) {
          candidates.Add(id);
          uint32_t tf = term_match.freqs[id];
          uint32_t doc_len = DocFieldLengthLocked(id, field);
          bm25_scores[id] += bm25_.Score(tf, doc_len, avg_len, N, df);
        }
      }
    }
  }

  // Term filters narrow the candidate set (AND semantics).
  for (const auto& term_filter : params.term_filters) {
    auto bitmap = TermMatchLocked(term_filter.field, ToLowerAscii(term_filter.value));
    candidates = dse::bitmap::RoaringBitmap::And(candidates, bitmap);
  }

  // Range filters: post-filter by reading each candidate's stored numeric
  // value (no on-disk numeric range index in this implementation — see
  // DOCUMENTATION.md "Range filter implementation").
  if (!params.range_filters.empty()) {
    dse::bitmap::RoaringBitmap survivors;
    for (uint32_t id : candidates.ToVector()) {
      auto src = GetSourceLocked(id);
      if (!src) continue;
      bool ok = true;
      try {
        auto doc = Document::Parse(*src);
        for (const auto& rf : params.range_filters) {
          auto num_val = doc.GetNumber(rf.field);
          if (!num_val) {
            ok = false;
            break;
          }
          if (rf.has_gte && *num_val < rf.gte) {
            ok = false;
            break;
          }
          if (rf.has_lte && *num_val > rf.lte) {
            ok = false;
            break;
          }
        }
      } catch (...) {
        ok = false;
      }
      if (ok) survivors.Add(id);
    }
    candidates = survivors;
  }

  // Vector search: results are always intersected with `candidates` so a
  // hybrid query's keyword/filter constraints are hard constraints, not
  // merely a re-ranking signal.
  std::unordered_map<uint32_t, double> vector_scores;
  if (params.has_vector) {
    auto vit = vector_indexes_.find(params.vector.field);
    if (vit != vector_indexes_.end()) {
      size_t k = static_cast<size_t>(std::max(params.vector.k, params.size + params.from));
      auto hits = vit->second->Search(params.vector.embedding, std::max<size_t>(k, 1),
                                       std::max<size_t>(k * 4, 50));
      for (const auto& [id, sim] : hits) {
        if (deleted_ids_.Contains(id)) continue;
        if (candidates.Contains(id)) vector_scores[id] = sim;
      }
    }
  }

  double max_bm25 = 0.0;
  if (params.has_vector && !params.query_text.empty()) {
    for (uint32_t id : candidates.ToVector()) {
      auto it = bm25_scores.find(id);
      if (it != bm25_scores.end()) max_bm25 = std::max(max_bm25, it->second);
    }
    if (max_bm25 <= 0.0) max_bm25 = 1.0;
  }

  std::vector<std::pair<uint32_t, double>> scored;
  scored.reserve(candidates.Cardinality());
  for (uint32_t id : candidates.ToVector()) {
    double keyword_component;
    if (params.query_text.empty()) {
      keyword_component = 1.0;
    } else {
      double raw = bm25_scores.count(id) ? bm25_scores[id] : 0.0;
      keyword_component = params.has_vector ? (raw / max_bm25) : raw;
    }
    double final_score = keyword_component;
    if (params.has_vector) {
      double vec_component = vector_scores.count(id) ? vector_scores[id] : 0.0;
      final_score = (1.0 - params.vector_weight) * keyword_component + params.vector_weight * vec_component;
    }
    scored.emplace_back(id, final_score);
  }

  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;  // deterministic tiebreak
  });

  result.total_hits = static_cast<int64_t>(scored.size());

  int from = std::max(params.from, 0);
  int size = params.size > 0 ? params.size : 10;
  for (int i = from; i < static_cast<int>(scored.size()) && i < from + size; i++) {
    uint32_t id = scored[i].first;
    SearchHitR hit;
    hit.doc_id = ResolveDocIdStringLocked(id).value_or("");
    hit.score = scored[i].second;
    hit.json = GetSourceLocked(id).value_or("{}");
    result.hits.push_back(std::move(hit));
  }

  // Aggregations run over the FULL filtered candidate set, not just the
  // paginated page — matching standard search-engine faceting semantics.
  if (!params.aggregations.empty()) {
    auto all_candidate_ids = candidates.ToVector();
    for (const auto& agg : params.aggregations) {
      dse::aggregation::ValueLookup lookup = [this, field = agg.field](uint32_t id)
          -> std::optional<dse::aggregation::FieldValue> {
        auto src = GetSourceLocked(id);
        if (!src) return std::nullopt;
        try {
          auto doc = Document::Parse(*src);
          auto num = doc.GetNumber(field);
          if (num) {
            dse::aggregation::FieldValue fv;
            fv.is_number = true;
            fv.num_val = *num;
            return fv;
          }
          auto str = doc.GetString(field);
          if (str) {
            dse::aggregation::FieldValue fv;
            fv.is_number = false;
            fv.str_val = *str;
            return fv;
          }
        } catch (...) {
        }
        return std::nullopt;
      };

      dse::aggregation::AggResult r;
      if (agg.type == "terms") {
        r = dse::aggregation::Aggregator::ComputeTerms(agg.name, all_candidate_ids, lookup, agg.size);
      } else if (agg.type == "stats") {
        r = dse::aggregation::Aggregator::ComputeStats(agg.name, all_candidate_ids, lookup);
      } else if (agg.type == "histogram") {
        r = dse::aggregation::Aggregator::ComputeHistogram(agg.name, all_candidate_ids, lookup,
                                                             agg.interval);
      } else {
        r.name = agg.name;
        r.type = agg.type;
      }
      result.aggregations.push_back(std::move(r));
    }
  }

  auto t1 = std::chrono::steady_clock::now();
  result.took_micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  return result;
}

// ---------------------------------------------------------------------------
// administration
// ---------------------------------------------------------------------------

std::string Shard::NextSegmentPath() {
  return data_dir_ + "/segment_" + std::to_string(next_segment_id_) + ".seg";
}

FlushResultR Shard::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  if (memtable_docs_.empty()) {
    return {true, "", -1};
  }
  try {
    dse::segment::SegmentBuildInput input;
    input.fields = memtable_index_.Fields();
    for (const auto& f : input.fields) input.avg_field_length[f] = memtable_index_.AvgFieldLength(f);
    input.postings = memtable_index_.AllPostings();
    input.doc_field_lengths = memtable_index_.AllDocFieldLengths();
    for (const auto& md : memtable_docs_) {
      input.docs.push_back({md.internal_id, md.doc_id, md.source_json});
    }

    std::string path = NextSegmentPath();
    int id = next_segment_id_;
    next_segment_id_++;
    dse::segment::Segment::Write(path, input);
    segments_.push_back(dse::segment::Segment::Open(path));

    memtable_docs_.clear();
    memtable_id_to_slot_.clear();
    memtable_index_ = dse::index::InvertedIndex();

    PersistTombstones();  // durable snapshot BEFORE truncating the WAL
    wal_->Truncate();

    return {true, "", id};
  } catch (const std::exception& e) {
    return {false, e.what(), -1};
  }
}

MergeResultR Shard::Merge() {
  std::lock_guard<std::mutex> lock(mu_);
  if (segments_.size() < 2) {
    return {true, "", 0, -1};
  }
  try {
    std::vector<std::string> input_paths;
    for (const auto& seg : segments_) input_paths.push_back(seg->Path());

    std::string output_path = NextSegmentPath();
    int out_id = next_segment_id_;
    next_segment_id_++;

    dse::segment::SegmentMerger::Merge(input_paths, output_path, deleted_ids_);

    int merged_count = static_cast<int>(segments_.size());
    for (const auto& p : input_paths) {
      std::error_code ec;
      fs::remove(p, ec);  // best-effort cleanup of old segment files
    }
    segments_.clear();
    segments_.push_back(dse::segment::Segment::Open(output_path));

    PersistTombstones();

    return {true, "", merged_count, out_id};
  } catch (const std::exception& e) {
    return {false, e.what(), 0, -1};
  }
}

ShardStatusR Shard::Status() {
  std::lock_guard<std::mutex> lock(mu_);
  ShardStatusR s;
  s.exists = true;
  s.doc_count = static_cast<int64_t>(TotalLiveDocsLocked());
  s.deleted_count = static_cast<int64_t>(deleted_ids_.Cardinality());
  s.segment_count = static_cast<int32_t>(segments_.size());
  s.wal_size_bytes = static_cast<int64_t>(wal_->SizeBytes());
  s.memtable_doc_count = static_cast<int64_t>(memtable_docs_.size());
  return s;
}

}  // namespace dse::shard
