#include "tokenizer.h"

#include <cctype>

namespace dse::analysis {

namespace {

// A character belongs to a word run if it is alphanumeric, or an internal
// apostrophe/hyphen. See the design note in tokenizer.h.
bool IsWordChar(unsigned char c) {
  return std::isalnum(c) || c == '\'' || c == '-';
}

// Trims leading/trailing apostrophes and hyphens from a raw run so that
// e.g. "well-known!" (whose run is "well-known" already, since '!' is not
// a word char) stays intact, while a run like "'quoted'" becomes "quoted"
// and a trailing hyphen run like "trailing-" becomes "trailing".
std::string TrimEdgeMarks(const std::string& raw) {
  size_t begin = 0;
  size_t end = raw.size();
  while (begin < end &&
         (raw[begin] == '\'' || raw[begin] == '-')) {
    ++begin;
  }
  while (end > begin &&
         (raw[end - 1] == '\'' || raw[end - 1] == '-')) {
    --end;
  }
  return raw.substr(begin, end - begin);
}

}  // namespace

std::vector<Token> Tokenizer::Tokenize(const std::string& text) {
  std::vector<Token> tokens;
  size_t i = 0;
  size_t position = 0;
  const size_t n = text.size();

  while (i < n) {
    // Skip non-word characters.
    while (i < n && !IsWordChar(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    if (i >= n) break;

    size_t start = i;
    while (i < n && IsWordChar(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    std::string raw = text.substr(start, i - start);
    std::string trimmed = TrimEdgeMarks(raw);

    // Discard tokens that are empty after trimming (e.g. a run of only
    // apostrophes/hyphens), or that would otherwise carry no alphanumeric
    // content.
    if (trimmed.empty()) continue;

    tokens.push_back(Token{trimmed, position});
    ++position;
  }

  return tokens;
}

}  // namespace dse::analysis
