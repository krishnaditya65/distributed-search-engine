#include "segment/segment.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <unistd.h>

#include "index/inverted_index.h"
#include "segment/segment_merger.h"

using namespace dse::segment;
using namespace dse::index;

static SegmentBuildInput BuildFrom(InvertedIndex& idx, std::vector<DocRecord> docs) {
  SegmentBuildInput input;
  input.fields = idx.Fields();
  for (const auto& f : input.fields) input.avg_field_length[f] = idx.AvgFieldLength(f);
  input.postings = idx.AllPostings();
  input.doc_field_lengths = idx.AllDocFieldLengths();
  input.docs = std::move(docs);
  return input;
}

static void TestWriteOpenRoundTrip() {
  InvertedIndex idx;
  idx.IndexField(1, "title", {"quick", "brown", "fox"});
  idx.IndexField(2, "title", {"lazy", "dog"});

  std::vector<DocRecord> docs = {
      {1, "doc-1", R"({"title":"quick brown fox"})"},
      {2, "doc-2", R"({"title":"lazy dog"})"},
  };
  auto input = BuildFrom(idx, docs);

  std::string path = "/tmp/dse_segment_test_" + std::to_string(getpid()) + ".seg";
  Segment::Write(path, input);

  auto seg = Segment::Open(path);
  assert(seg->DocCount() == 2);
  const auto* p = seg->GetPostings("title", "quick");
  assert(p != nullptr);
  assert(p->doc_ids.ToVector() == std::vector<uint32_t>({1}));
  assert(seg->DocFreq("title", "dog") == 1);
  assert(seg->DocFieldLength(1, "title") == 3);
  assert(seg->AvgFieldLength("title") > 2.4 && seg->AvgFieldLength("title") < 2.6);  // (3+2)/2=2.5

  auto src = seg->GetSource(1);
  assert(src.has_value() && src->find("quick") != std::string::npos);

  auto id = seg->ResolveDocID("doc-2");
  assert(id.has_value() && *id == 2);
  auto docid = seg->ResolveInternalID(2);
  assert(docid.has_value() && *docid == "doc-2");

  assert(!seg->ResolveDocID("nope").has_value());

  unlink(path.c_str());
  std::cout << "TestWriteOpenRoundTrip passed\n";
}

static void TestMergeDropsDeletedAndCombinesSegments() {
  InvertedIndex idx1;
  idx1.IndexField(1, "title", {"apple", "pie"});
  idx1.IndexField(2, "title", {"banana", "split"});
  std::string p1 = "/tmp/dse_segment_test_a_" + std::to_string(getpid()) + ".seg";
  Segment::Write(p1, BuildFrom(idx1, {{1, "d1", R"({"title":"apple pie"})"},
                                       {2, "d2", R"({"title":"banana split"})"}}));

  InvertedIndex idx2;
  idx2.IndexField(3, "title", {"apple", "tart"});
  std::string p2 = "/tmp/dse_segment_test_b_" + std::to_string(getpid()) + ".seg";
  Segment::Write(p2, BuildFrom(idx2, {{3, "d3", R"({"title":"apple tart"})"}}));

  dse::bitmap::RoaringBitmap deleted;
  deleted.Add(2);  // doc 2 (banana split) should be dropped by the merge

  std::string out = "/tmp/dse_segment_test_merged_" + std::to_string(getpid()) + ".seg";
  SegmentMerger::Merge({p1, p2}, out, deleted);

  auto merged = Segment::Open(out);
  assert(merged->DocCount() == 2);  // doc 1 and doc 3 survive
  assert(merged->DocFreq("title", "apple") == 2);
  assert(merged->DocFreq("title", "banana") == 0);  // fully gone, doc 2 was its only occurrence
  assert(!merged->GetSource(2).has_value());
  assert(merged->GetSource(1).has_value());
  assert(merged->GetSource(3).has_value());

  unlink(p1.c_str());
  unlink(p2.c_str());
  unlink(out.c_str());
  std::cout << "TestMergeDropsDeletedAndCombinesSegments passed\n";
}

int main() {
  TestWriteOpenRoundTrip();
  TestMergeDropsDeletedAndCombinesSegments();
  std::cout << "segment tests passed\n";
  return 0;
}
