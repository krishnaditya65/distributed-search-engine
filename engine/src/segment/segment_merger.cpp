#include "segment/segment_merger.h"

#include <set>
#include <stdexcept>

#include "segment/segment.h"

namespace dse::segment {

void SegmentMerger::Merge(const std::vector<std::string>& input_paths,
                           const std::string& output_path,
                           const dse::bitmap::RoaringBitmap& deleted) {
  if (input_paths.empty()) {
    throw std::runtime_error("SegmentMerger::Merge called with no input segments");
  }

  std::vector<std::unique_ptr<Segment>> segments;
  segments.reserve(input_paths.size());
  for (const auto& p : input_paths) {
    segments.push_back(Segment::Open(p));
  }

  SegmentBuildInput out;
  std::set<std::string> field_set;

  // Pass 1: surviving documents (dedupe by internal id in case, hypothetically,
  // the same id appeared in more than one input segment — shouldn't happen
  // under this engine's id-assignment policy, but guard against it anyway).
  std::set<uint32_t> seen_ids;
  for (const auto& seg : segments) {
    for (uint32_t id : seg->AllDocIDs()) {
      if (deleted.Contains(id) || seen_ids.count(id)) continue;
      seen_ids.insert(id);
      DocRecord rec;
      rec.internal_id = id;
      rec.doc_id = seg->ResolveInternalID(id).value_or("");
      rec.source_json = seg->GetSource(id).value_or("{}");
      out.docs.push_back(std::move(rec));
    }
  }

  // Pass 2: merge postings per (field, term), keeping only surviving doc ids,
  // and recompute per-field doc lengths / averages from survivors only.
  std::unordered_map<std::string, std::unordered_map<std::string, dse::index::Postings>> merged_postings;
  std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>> merged_lengths;
  std::unordered_map<std::string, uint64_t> field_total_len;
  std::unordered_map<std::string, uint64_t> field_doc_count;

  for (const auto& seg : segments) {
    for (const auto& field : seg->Fields()) {
      field_set.insert(field);
    }
  }

  // Merge postings and recompute per-field doc lengths from surviving docs.
  for (const auto& seg : segments) {
    seg->ForEachPosting([&](const std::string& field, const std::string& term,
                             const dse::index::Postings& postings) {
      auto& merged = merged_postings[field][term];
      auto ids = postings.doc_ids.ToVector();
      for (size_t i = 0; i < ids.size(); i++) {
        uint32_t id = ids[i];
        if (deleted.Contains(id) || !seen_ids.count(id)) continue;
        merged.doc_ids.Add(id);
        merged.term_freqs.push_back(postings.term_freqs[i]);
      }
    });

    for (uint32_t id : seg->AllDocIDs()) {
      if (deleted.Contains(id)) continue;
      for (const auto& field : seg->Fields()) {
        uint32_t len = seg->DocFieldLength(id, field);
        if (len == 0) continue;
        merged_lengths[field][id] = len;
        field_total_len[field] += len;
        field_doc_count[field] += 1;
      }
    }
  }

  out.fields.assign(field_set.begin(), field_set.end());
  for (const auto& field : out.fields) {
    uint64_t cnt = field_doc_count.count(field) ? field_doc_count[field] : 0;
    out.avg_field_length[field] =
        cnt == 0 ? 0.0 : static_cast<double>(field_total_len[field]) / static_cast<double>(cnt);
  }
  out.postings = std::move(merged_postings);
  out.doc_field_lengths = std::move(merged_lengths);

  Segment::Write(output_path, out);
}

}  // namespace dse::segment
