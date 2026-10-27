#ifndef DSE_ANALYSIS_TOKENIZER_H_
#define DSE_ANALYSIS_TOKENIZER_H_

#include <string>
#include <vector>

namespace dse::analysis {

// A single token extracted from raw text along with its ordinal position
// within the sequence of tokens produced for that text.
struct Token {
  std::string text;
  size_t position;  // 0-based ordinal position of this token within the field
};

// Splits raw text into word tokens.
//
// Design choice: internal apostrophes and hyphens are kept as part of a
// token (so "don't" stays "don't" and "well-known" stays "well-known")
// because treating them as separate words would lose meaningful compound
// terms that are common in English text. However, a token is trimmed of
// any leading/trailing apostrophes or hyphens, since those are almost
// always punctuation (e.g. quotes around a word, or a trailing hyphen at
// a line break) rather than part of the word itself. Pure
// punctuation/whitespace runs are discarded entirely.
class Tokenizer {
 public:
  static std::vector<Token> Tokenize(const std::string& text);
};

}  // namespace dse::analysis

#endif  // DSE_ANALYSIS_TOKENIZER_H_
