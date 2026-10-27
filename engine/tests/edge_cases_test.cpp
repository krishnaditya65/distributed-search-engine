#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "bitmap/roaring_bitmap.h"
#include "document/document.h"
#include "index/inverted_index.h"
#include "scoring/bm25.h"
#include "storage/mmap_file.h"
#include "wal/wal.h"

namespace fs = std::filesystem;

static std::string TmpPath(const std::string& name) {
  return "/tmp/dse_edge_cases_" + std::to_string(getpid()) + "_" + name;
}

static void TestDocumentTypesAndValidation() {
  auto doc = dse::document::Document::Parse(
      R"({"name":"Ada","count":7,"enabled":true,"tags":["one","two"],"embedding":[1,2.5,false]})");
  assert(doc.GetString("name") == std::optional<std::string>("Ada"));
  assert(doc.GetNumber("count") == std::optional<double>(7.0));
  assert(doc.GetNumber("enabled") == std::optional<double>(1.0));
  assert(doc.GetAsIndexableString("tags") == std::optional<std::string>("one two"));
  assert(doc.GetAsIndexableString("missing") == std::nullopt);
  assert(doc.GetVector("embedding") == std::nullopt);  // bool is not a vector number
  assert(doc.GetVector("nope") == std::nullopt);

  bool invalid_json = false;
  try {
    (void)dse::document::Document::Parse("{not-json");
  } catch (const std::invalid_argument&) {
    invalid_json = true;
  }
  assert(invalid_json);

  bool non_object = false;
  try {
    (void)dse::document::Document::Parse("[1,2,3]");
  } catch (const std::invalid_argument&) {
    non_object = true;
  }
  assert(non_object);
  std::cout << "TestDocumentTypesAndValidation passed\n";
}

static void TestBitmapBoundariesDenseAndSerialization() {
  dse::bitmap::RoaringBitmap bitmap;
  for (uint32_t value : {0u, 1u, 65535u, 65536u, 0xFFFFFFFFu}) bitmap.Add(value);
  bitmap.Add(65535u);  // duplicate must not change cardinality
  assert(bitmap.Cardinality() == 5);
  assert(bitmap.ToVector() == std::vector<uint32_t>({0u, 1u, 65535u, 65536u, 0xFFFFFFFFu}));

  dse::bitmap::RoaringBitmap dense;
  for (uint32_t value = 0; value <= 4096; ++value) dense.Add(value);
  assert(dense.Cardinality() == 4097);
  dense.Remove(0);
  dense.Remove(4096);
  assert(!dense.Contains(0));
  assert(!dense.Contains(4096));
  assert(dense.Cardinality() == 4095);

  auto unioned = dse::bitmap::RoaringBitmap::Or(bitmap, dense);
  auto encoded = unioned.Serialize();
  auto decoded = dse::bitmap::RoaringBitmap::Deserialize(encoded);
  assert(decoded.ToVector() == unioned.ToVector());
  assert(dse::bitmap::RoaringBitmap::And(bitmap, dense).Contains(1));
  assert(dse::bitmap::RoaringBitmap::AndNot(bitmap, dense).Contains(0xFFFFFFFFu));
  std::cout << "TestBitmapBoundariesDenseAndSerialization passed\n";
}

static void TestWalTornTailAndTruncate() {
  const std::string path = TmpPath("wal");
  fs::remove(path);
  {
    dse::wal::WriteAheadLog wal(path);
    wal.Append({dse::wal::WalOpType::kIndex, "a", R"({"x":1})"});
    wal.Append({dse::wal::WalOpType::kDelete, "b", ""});
    assert(wal.ReadAll().size() == 2);
    assert(wal.SizeBytes() > 0);
    wal.Truncate();
    assert(wal.SizeBytes() == 0);
  }
  {
    dse::wal::WriteAheadLog wal(path);
    wal.Append({dse::wal::WalOpType::kIndex, "good", R"({"ok":true})"});
  }
  {
    std::ofstream tail(path, std::ios::binary | std::ios::app);
    tail.write("\x05\x00", 2);  // incomplete next frame
  }
  dse::wal::WriteAheadLog recovered(path);
  auto records = recovered.ReadAll();
  assert(records.size() == 1);
  assert(records[0].doc_id == "good");
  fs::remove(path);
  std::cout << "TestWalTornTailAndTruncate passed\n";
}

static void TestMMapMoveReadOnlyAndInvalidSizes() {
  const std::string path = TmpPath("mmap");
  fs::remove(path);
  auto mapped = dse::storage::MMapFile::CreateAndMap(path, 8);
  assert(mapped.IsWritable());
  mapped.Data()[0] = 0xAB;
  mapped.Data()[7] = 0xCD;
  mapped.Sync();
  auto moved = std::move(mapped);
  assert(mapped.Size() == 0);
  assert(moved.Size() == 8);
  {
    auto read_only = dse::storage::MMapFile::OpenReadOnly(path);
    assert(!read_only.IsWritable());
    assert(read_only.Data()[0] == 0xAB);
    assert(read_only.Data()[7] == 0xCD);
  }
  bool zero_rejected = false;
  try {
    (void)dse::storage::MMapFile::CreateAndMap(TmpPath("zero"), 0);
  } catch (const std::runtime_error&) {
    zero_rejected = true;
  }
  assert(zero_rejected);
  fs::remove(path);
  std::cout << "TestMMapMoveReadOnlyAndInvalidSizes passed\n";
}

static void TestInvertedIndexStatisticsAndBm25Edges() {
  dse::index::InvertedIndex index;
  index.IndexField(7, "title", {"quick", "quick", "fox"});
  index.IndexField(9, "title", {"fox"});
  assert(index.DocFreq("title", "quick") == 1);
  assert(index.DocFreq("title", "fox") == 2);
  assert(index.DocFieldLength(7, "title") == 3);
  assert(index.FieldDocCount("title") == 2);
  assert(std::abs(index.AvgFieldLength("title") - 2.0) < 1e-9);
  const auto* postings = index.GetPostings("title", "quick");
  assert(postings != nullptr);
  assert(postings->term_freqs.size() == 1 && postings->term_freqs[0] == 2);
  assert(index.GetPostings("title", "missing") == nullptr);

  dse::scoring::BM25Scorer scorer;
  assert(scorer.Idf(10, 0) > scorer.Idf(10, 10));
  assert(scorer.Score(2, 3, 2.0, 10, 1) > scorer.Score(1, 3, 2.0, 10, 1));
  std::cout << "TestInvertedIndexStatisticsAndBm25Edges passed\n";
}

int main() {
  TestDocumentTypesAndValidation();
  TestBitmapBoundariesDenseAndSerialization();
  TestWalTornTailAndTruncate();
  TestMMapMoveReadOnlyAndInvalidSizes();
  TestInvertedIndexStatisticsAndBm25Edges();
  return 0;
}
