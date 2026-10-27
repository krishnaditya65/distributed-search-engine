#ifndef DSE_ANALYSIS_ANALYZER_H_
#define DSE_ANALYSIS_ANALYZER_H_

#include <string>
#include <vector>

namespace dse::analysis {

// Runs raw field text through the full analysis pipeline used to produce
// index-ready terms: tokenize -> lowercase -> stopword removal.
class Analyzer {
 public:
  Analyzer();

  // Runs the full pipeline: tokenize -> lowercase -> stopword removal.
  // Returns the final ordered list of index-ready terms (duplicates ARE
  // kept -- term frequency depends on it). Empty input returns an empty
  // vector.
  std::vector<std::string> Analyze(const std::string& text) const;

 private:
  // Lowercases a single token's text. ASCII-only casefolding (via
  // std::tolower per byte) -- not full Unicode casefolding.
  static std::string LowercaseFilter(const std::string& token_text);

  // Returns true if the (already-lowercased) term should be dropped
  // because it is a stopword or empty.
  static bool IsStopword(const std::string& lowercased_term);
};

}  // namespace dse::analysis

#endif  // DSE_ANALYSIS_ANALYZER_H_
