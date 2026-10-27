#include "index/inverted_index.h"

namespace dse::index {

void InvertedIndex::IndexField(uint32_t doc_id, const std::string& field,
                                const std::vector<std::string>& terms) {
  // Term frequency = number of occurrences of each unique term in `terms`.
  std::unordered_map<std::string, uint32_t> tf;
  tf.reserve(terms.size());
  for (const auto& t : terms) {
    tf[t]++;
  }

  auto& field_postings = postings_[field];
  for (const auto& [term, freq] : tf) {
    Postings& p = field_postings[term];
    p.doc_ids.Add(doc_id);
    // doc_ids.ToVector() is always ascending; since doc ids are assigned
    // monotonically increasing by the shard and each (doc,field) pair is
    // indexed at most once, each new Add() here appends the largest id seen
    // so far for this term, which lands at the END of ToVector(). We can
    // safely push_back to keep term_freqs aligned without re-deriving the
    // full order on every insert.
    p.term_freqs.push_back(freq);
  }

  doc_field_lengths_[field][doc_id] = static_cast<uint32_t>(terms.size());
  field_total_length_[field] += terms.size();
  field_doc_count_[field] += 1;
}

const Postings* InvertedIndex::GetPostings(const std::string& field,
                                            const std::string& term) const {
  auto fit = postings_.find(field);
  if (fit == postings_.end()) return nullptr;
  auto tit = fit->second.find(term);
  if (tit == fit->second.end()) return nullptr;
  return &tit->second;
}

uint32_t InvertedIndex::DocFieldLength(uint32_t doc_id, const std::string& field) const {
  auto fit = doc_field_lengths_.find(field);
  if (fit == doc_field_lengths_.end()) return 0;
  auto dit = fit->second.find(doc_id);
  if (dit == fit->second.end()) return 0;
  return dit->second;
}

double InvertedIndex::AvgFieldLength(const std::string& field) const {
  auto cit = field_doc_count_.find(field);
  if (cit == field_doc_count_.end() || cit->second == 0) return 0.0;
  auto tit = field_total_length_.find(field);
  double total = (tit == field_total_length_.end()) ? 0.0 : static_cast<double>(tit->second);
  return total / static_cast<double>(cit->second);
}

uint64_t InvertedIndex::DocFreq(const std::string& field, const std::string& term) const {
  const Postings* p = GetPostings(field, term);
  return p == nullptr ? 0 : p->doc_ids.Cardinality();
}

uint64_t InvertedIndex::FieldDocCount(const std::string& field) const {
  auto it = field_doc_count_.find(field);
  return it == field_doc_count_.end() ? 0 : it->second;
}

std::vector<std::string> InvertedIndex::Fields() const {
  std::vector<std::string> out;
  out.reserve(postings_.size());
  for (const auto& [field, _] : postings_) {
    out.push_back(field);
  }
  return out;
}

}  // namespace dse::index
