#include "bm25.h"

#include <cmath>

namespace dse::scoring {

BM25Scorer::BM25Scorer(BM25Params params) : params_(params) {}

double BM25Scorer::Idf(uint64_t total_docs, uint64_t doc_freq) const {
  // We use the "+1 inside the log" smoothed variant
  //   idf = ln(1 + (N - df + 0.5) / (df + 0.5))
  // rather than the classic Robertson/Sparck-Jones form
  //   idf = ln((N - df + 0.5) / (df + 0.5))
  // The classic form goes negative whenever a term appears in more than
  // half the corpus (df > N/2), which would make such terms actively hurt
  // a document's score. Adding 1 before taking the log guarantees the
  // argument is always >= 1, so idf is always >= 0 for any valid
  // 0 <= df <= N, N >= 1.
  double n = static_cast<double>(total_docs);
  double df = static_cast<double>(doc_freq);
  double ratio = (n - df + 0.5) / (df + 0.5);
  return std::log(1.0 + ratio);
}

double BM25Scorer::Score(uint32_t term_freq, uint32_t doc_length, double avg_doc_length,
                          uint64_t total_docs, uint64_t doc_freq) const {
  double idf = Idf(total_docs, doc_freq);
  double tf = static_cast<double>(term_freq);

  // Degenerate case: no meaningful average length to normalize against.
  // Treat the length-normalization factor as neutral (1.0).
  double length_norm = 1.0;
  if (avg_doc_length > 0.0) {
    length_norm = (1.0 - params_.b) + params_.b * (static_cast<double>(doc_length) / avg_doc_length);
  }

  double denominator = tf + params_.k1 * length_norm;
  if (denominator == 0.0) {
    return 0.0;
  }

  return idf * (tf * (params_.k1 + 1.0)) / denominator;
}

}  // namespace dse::scoring
