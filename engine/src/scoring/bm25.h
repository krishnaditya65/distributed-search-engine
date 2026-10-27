#ifndef DSE_SCORING_BM25_H_
#define DSE_SCORING_BM25_H_

#include <cstdint>

namespace dse::scoring {

// Tunable parameters for the Okapi BM25 scoring function.
struct BM25Params {
  double k1 = 1.2;  // controls term-frequency saturation
  double b = 0.75;  // controls document-length normalization strength (0 = none, 1 = full)
};

// Computes Okapi BM25 relevance scores for (term, document) pairs.
class BM25Scorer {
 public:
  explicit BM25Scorer(BM25Params params = BM25Params{});

  // Inverse document frequency for a term, using the standard Lucene/ES
  // "BM25+" smoothed variant that stays non-negative even for terms present
  // in the majority of documents:
  //   idf = ln(1 + (N - df + 0.5) / (df + 0.5))
  // where N = total_docs, df = doc_freq (number of docs containing the term).
  double Idf(uint64_t total_docs, uint64_t doc_freq) const;

  // Full BM25 contribution of a single term to a single document's score:
  //   score = idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * (doc_length / avg_doc_length)))
  // where tf = term_freq (how many times the term occurs in this document's field),
  // doc_length = this document's field length in tokens,
  // avg_doc_length = average field length across all documents in the shard for that field.
  double Score(uint32_t term_freq, uint32_t doc_length, double avg_doc_length,
               uint64_t total_docs, uint64_t doc_freq) const;

 private:
  BM25Params params_;
};

}  // namespace dse::scoring

#endif  // DSE_SCORING_BM25_H_
