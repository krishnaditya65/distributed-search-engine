#include "analyzer.h"

#include <cctype>
#include <unordered_set>

#include "tokenizer.h"

namespace dse::analysis {

namespace {

// A reasonably standard English stopword list (common function words that
// carry little value as index terms). Not exhaustive, but covers the most
// frequent offenders.
const std::unordered_set<std::string>& StopwordSet() {
  static const std::unordered_set<std::string> kStopwords = {
      "a",    "an",   "and",  "are",  "as",   "at",   "be",   "by",
      "for",  "from", "has",  "he",   "in",   "is",   "it",   "its",
      "of",   "on",   "that", "the",  "to",   "was",  "were", "will",
      "with", "this", "but",  "they", "have", "had",  "not",  "or",
      "we",   "you",  "your", "i",    "his",  "her",  "she",  "them",
      "their","what", "which","who",  "when", "where","why",  "how",
      "all",  "each", "so",   "than", "too",  "can",  "if",   "no",
  };
  return kStopwords;
}

}  // namespace

Analyzer::Analyzer() = default;

std::string Analyzer::LowercaseFilter(const std::string& token_text) {
  // ASCII-only casefolding: std::tolower is applied per byte. This is not
  // full Unicode casefolding, which is out of scope for this ASCII-oriented
  // module.
  std::string lowered;
  lowered.reserve(token_text.size());
  for (unsigned char c : token_text) {
    lowered.push_back(static_cast<char>(std::tolower(c)));
  }
  return lowered;
}

bool Analyzer::IsStopword(const std::string& lowercased_term) {
  if (lowercased_term.empty()) return true;
  return StopwordSet().find(lowercased_term) != StopwordSet().end();
}

std::vector<std::string> Analyzer::Analyze(const std::string& text) const {
  std::vector<std::string> terms;
  std::vector<Token> tokens = Tokenizer::Tokenize(text);
  terms.reserve(tokens.size());

  for (const Token& token : tokens) {
    std::string lowered = LowercaseFilter(token.text);
    if (IsStopword(lowered)) continue;
    terms.push_back(std::move(lowered));
  }

  return terms;
}

}  // namespace dse::analysis
