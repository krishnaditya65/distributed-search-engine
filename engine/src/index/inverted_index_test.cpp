#include "index/inverted_index.h"

#include <cassert>
#include <iostream>

using dse::index::InvertedIndex;
using dse::index::Postings;

static void TestBasicIndexAndLookup() {
  InvertedIndex idx;
  idx.IndexField(1, "title", {"quick", "brown", "fox", "quick"});
  idx.IndexField(2, "title", {"lazy", "dog"});
  idx.IndexField(3, "title", {"quick", "dog"});

  const Postings* p = idx.GetPostings("title", "quick");
  assert(p != nullptr);
  assert(p->doc_ids.Cardinality() == 2);
  auto ids = p->doc_ids.ToVector();
  assert(ids.size() == 2 && ids[0] == 1 && ids[1] == 3);
  // doc 1 has "quick" twice, doc 3 has it once; alignment follows ascending doc id order.
  assert(p->term_freqs[0] == 2);
  assert(p->term_freqs[1] == 1);

  assert(idx.GetPostings("title", "nonexistent") == nullptr);
  assert(idx.GetPostings("missingfield", "quick") == nullptr);

  assert(idx.DocFreq("title", "dog") == 2);
  assert(idx.DocFreq("title", "fox") == 1);
  assert(idx.DocFreq("title", "nope") == 0);

  std::cout << "TestBasicIndexAndLookup passed\n";
}

static void TestFieldLengthsAndAverage() {
  InvertedIndex idx;
  idx.IndexField(1, "body", {"a", "b", "c"});        // length 3
  idx.IndexField(2, "body", {"a", "b", "c", "d", "e"}); // length 5
  idx.IndexField(3, "body", {"a"});                    // length 1

  assert(idx.DocFieldLength(1, "body") == 3);
  assert(idx.DocFieldLength(2, "body") == 5);
  assert(idx.DocFieldLength(3, "body") == 1);
  assert(idx.DocFieldLength(999, "body") == 0);  // unknown doc -> 0
  assert(idx.DocFieldLength(1, "nofield") == 0); // unknown field -> 0

  double avg = idx.AvgFieldLength("body");
  assert(avg > 2.99 && avg < 3.01);  // (3+5+1)/3 = 3.0

  assert(idx.AvgFieldLength("nofield") == 0.0);

  std::cout << "TestFieldLengthsAndAverage passed\n";
}

static void TestMultipleFieldsIndependent() {
  InvertedIndex idx;
  idx.IndexField(1, "title", {"apple"});
  idx.IndexField(1, "body", {"apple", "pie", "apple"});

  assert(idx.DocFreq("title", "apple") == 1);
  assert(idx.DocFreq("body", "apple") == 1);  // 1 doc, even though term_freq within is 2
  const Postings* body_apple = idx.GetPostings("body", "apple");
  assert(body_apple != nullptr);
  assert(body_apple->term_freqs[0] == 2);

  auto fields = idx.Fields();
  assert(fields.size() == 2);

  std::cout << "TestMultipleFieldsIndependent passed\n";
}

int main() {
  TestBasicIndexAndLookup();
  TestFieldLengthsAndAverage();
  TestMultipleFieldsIndependent();
  std::cout << "inverted index tests passed\n";
  return 0;
}
