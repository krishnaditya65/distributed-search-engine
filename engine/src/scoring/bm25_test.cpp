#include "bm25.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

constexpr double kEps = 1e-9;

bool NearlyEqual(double a, double b, double eps = kEps) {
  return std::fabs(a - b) <= eps;
}

void TestIdfHandComputed() {
  dse::scoring::BM25Scorer scorer;

  // total_docs = 100, doc_freq = 10
  // idf = ln(1 + (100 - 10 + 0.5) / (10 + 0.5)) = ln(1 + 90.5 / 10.5)
  {
    double expected = std::log(1.0 + 90.5 / 10.5);
    double actual = scorer.Idf(100, 10);
    assert(NearlyEqual(actual, expected));
  }

  // total_docs = 50, doc_freq = 1
  // idf = ln(1 + (50 - 1 + 0.5) / (1 + 0.5)) = ln(1 + 49.5 / 1.5)
  {
    double expected = std::log(1.0 + 49.5 / 1.5);
    double actual = scorer.Idf(50, 1);
    assert(NearlyEqual(actual, expected));
  }
}

void TestScoreHandComputed() {
  dse::scoring::BM25Params params;
  params.k1 = 1.2;
  params.b = 0.75;
  dse::scoring::BM25Scorer scorer(params);

  uint64_t total_docs = 100;
  uint64_t doc_freq = 10;
  uint32_t term_freq = 3;
  uint32_t doc_length = 120;
  double avg_doc_length = 100.0;

  double idf = std::log(1.0 + (100.0 - 10.0 + 0.5) / (10.0 + 0.5));
  double length_norm = (1.0 - params.b) + params.b * (doc_length / avg_doc_length);
  double expected =
      idf * (term_freq * (params.k1 + 1.0)) / (term_freq + params.k1 * length_norm);

  double actual = scorer.Score(term_freq, doc_length, avg_doc_length, total_docs, doc_freq);
  assert(NearlyEqual(actual, expected));
}

void TestTermFrequencyMonotonicityWithDiminishingReturns() {
  dse::scoring::BM25Scorer scorer;
  uint32_t doc_length = 100;
  double avg_doc_length = 100.0;
  uint64_t total_docs = 1000;
  uint64_t doc_freq = 50;

  double s1 = scorer.Score(1, doc_length, avg_doc_length, total_docs, doc_freq);
  double s2 = scorer.Score(2, doc_length, avg_doc_length, total_docs, doc_freq);
  double s9 = scorer.Score(9, doc_length, avg_doc_length, total_docs, doc_freq);
  double s10 = scorer.Score(10, doc_length, avg_doc_length, total_docs, doc_freq);

  // Strictly increasing with term frequency.
  assert(s2 > s1);
  assert(s9 > s2);
  assert(s10 > s9);

  // Diminishing returns: the increment from 1->2 must be strictly larger
  // than the increment from 9->10.
  double increment_low = s2 - s1;
  double increment_high = s10 - s9;
  assert(increment_low > increment_high);
}

void TestLengthNormalization() {
  uint64_t total_docs = 1000;
  uint64_t doc_freq = 50;
  uint32_t term_freq = 5;
  double avg_doc_length = 100.0;

  // With b > 0, a much longer document should score lower.
  {
    dse::scoring::BM25Params params;
    params.k1 = 1.2;
    params.b = 0.75;
    dse::scoring::BM25Scorer scorer(params);

    double score_avg = scorer.Score(term_freq, 100, avg_doc_length, total_docs, doc_freq);
    double score_long = scorer.Score(term_freq, 1000, avg_doc_length, total_docs, doc_freq);
    assert(score_long < score_avg);
  }

  // With b == 0, document length must have no effect on score at all.
  {
    dse::scoring::BM25Params params;
    params.k1 = 1.2;
    params.b = 0.0;
    dse::scoring::BM25Scorer scorer(params);

    double score_short = scorer.Score(term_freq, 10, avg_doc_length, total_docs, doc_freq);
    double score_avg = scorer.Score(term_freq, 100, avg_doc_length, total_docs, doc_freq);
    double score_long = scorer.Score(term_freq, 10000, avg_doc_length, total_docs, doc_freq);

    assert(NearlyEqual(score_short, score_avg));
    assert(NearlyEqual(score_avg, score_long));
  }
}

void TestIdfNonNegativity() {
  dse::scoring::BM25Scorer scorer;
  uint64_t total_docs = 200;

  for (uint64_t doc_freq = 0; doc_freq <= total_docs; doc_freq += 7) {
    double idf = scorer.Idf(total_docs, doc_freq);
    assert(idf >= 0.0);
  }

  // Edge case: term appears in every single document.
  double idf_all = scorer.Idf(total_docs, total_docs);
  assert(idf_all >= 0.0);

  // Edge case: term appears in no documents.
  double idf_none = scorer.Idf(total_docs, 0);
  assert(idf_none >= 0.0);
}

void TestDegenerateInputsDoNotCrash() {
  dse::scoring::BM25Scorer scorer;

  // avg_doc_length <= 0 should be treated as length-norm factor 1.0, not crash.
  double score = scorer.Score(3, 50, 0.0, 100, 10);
  assert(std::isfinite(score));

  double score_neg = scorer.Score(3, 50, -10.0, 100, 10);
  assert(std::isfinite(score_neg));

  // term_freq == 0 should not crash and should yield score 0.
  double score_zero_tf = scorer.Score(0, 50, 100.0, 100, 10);
  assert(NearlyEqual(score_zero_tf, 0.0));
}

}  // namespace

int main() {
  TestIdfHandComputed();
  TestScoreHandComputed();
  TestTermFrequencyMonotonicityWithDiminishingReturns();
  TestLengthNormalization();
  TestIdfNonNegativity();
  TestDegenerateInputsDoNotCrash();

  std::cout << "bm25 tests passed" << std::endl;
  return 0;
}
