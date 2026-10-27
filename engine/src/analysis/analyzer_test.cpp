#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "analyzer.h"
#include "tokenizer.h"

using dse::analysis::Analyzer;
using dse::analysis::Token;
using dse::analysis::Tokenizer;

namespace {

void TestAnalyzeBasicSentence() {
  Analyzer analyzer;
  std::vector<std::string> result =
      analyzer.Analyze("The Quick Brown Fox jumps over the lazy dog!");
  std::vector<std::string> expected = {"quick", "brown", "fox", "jumps",
                                        "over",  "lazy",  "dog"};
  assert(result == expected);
}

void TestAnalyzeEmptyInput() {
  Analyzer analyzer;
  std::vector<std::string> result = analyzer.Analyze("");
  assert(result.empty());
}

void TestAnalyzeOnlyStopwordsAndPunctuation() {
  Analyzer analyzer;
  std::vector<std::string> result = analyzer.Analyze("The, and; of -- the!");
  assert(result.empty());
}

void TestTokenizeApostropheAndHyphen() {
  std::vector<Token> tokens =
      Tokenizer::Tokenize("Well-known: don't stop, well-known!");

  // Expect 4 tokens: "Well-known", "don't", "stop", "well-known"
  assert(tokens.size() == 4);

  assert(tokens[0].text == "Well-known");
  assert(tokens[0].position == 0);

  assert(tokens[1].text == "don't");
  assert(tokens[1].position == 1);

  assert(tokens[2].text == "stop");
  assert(tokens[2].position == 2);

  assert(tokens[3].text == "well-known");
  assert(tokens[3].position == 3);

  // Increasing positions.
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    assert(tokens[i].position + 1 == tokens[i + 1].position);
  }
}

void TestTokenizeTrimsEdgeMarks() {
  // Leading/trailing apostrophes and hyphens should be trimmed off,
  // while internal ones are preserved.
  std::vector<Token> tokens = Tokenizer::Tokenize("'quoted' -trailing- 'a'");
  assert(tokens.size() == 3);
  assert(tokens[0].text == "quoted");
  assert(tokens[1].text == "trailing");
  assert(tokens[2].text == "a");
}

}  // namespace

int main() {
  TestAnalyzeBasicSentence();
  TestAnalyzeEmptyInput();
  TestAnalyzeOnlyStopwordsAndPunctuation();
  TestTokenizeApostropheAndHyphen();
  TestTokenizeTrimsEdgeMarks();

  std::cout << "analyzer tests passed" << std::endl;
  return 0;
}
