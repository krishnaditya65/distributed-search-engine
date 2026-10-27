#include "shard/shard.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <filesystem>
#include <unistd.h>

using namespace dse::shard;
using dse::document::FieldMapping;

namespace fs = std::filesystem;

static std::string TmpDir(const std::string& suffix) {
  return "/tmp/dse_shard_test_" + std::to_string(getpid()) + "_" + suffix;
}

static std::vector<FieldMapping> ProductMappings() {
  return {
      {"title", "text", 0},
      {"brand", "keyword", 0},
      {"price", "number", 0},
      {"embedding", "vector", 4},
  };
}

static void TestCRUD() {
  std::string dir = TmpDir("crud");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  auto r1 = shard.Index("p1", R"({"title":"Red Running Shoes","brand":"Nike","price":89})");
  assert(r1.success);
  assert(r1.doc_id == "p1");

  auto g1 = shard.Get("p1");
  assert(g1.found);
  assert(g1.json.find("Nike") != std::string::npos);

  auto g2 = shard.Get("nonexistent");
  assert(!g2.found);

  // update: re-index same doc_id with new content
  auto r2 = shard.Index("p1", R"({"title":"Red Running Shoes V2","brand":"Nike","price":95})");
  assert(r2.success);
  auto g3 = shard.Get("p1");
  assert(g3.json.find("V2") != std::string::npos);
  assert(g3.json.find("95") != std::string::npos);

  auto d1 = shard.Delete("p1");
  assert(d1.found);
  auto g4 = shard.Get("p1");
  assert(!g4.found);
  auto d2 = shard.Delete("p1");
  assert(!d2.found);  // already deleted

  fs::remove_all(dir);
  std::cout << "TestCRUD passed\n";
}

static void TestBulk() {
  std::string dir = TmpDir("bulk");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  std::vector<BulkOpInput> ops = {
      {false, "b1", R"({"title":"Item One","brand":"Acme","price":10})"},
      {false, "b2", R"({"title":"Item Two","brand":"Acme","price":20})"},
      {true, "b1", ""},
  };
  auto results = shard.Bulk(ops);
  assert(results.size() == 3);
  assert(results[0].success);
  assert(results[1].success);
  assert(results[2].success);  // delete of b1 succeeds since b1 was indexed by op 0

  assert(!shard.Get("b1").found);
  assert(shard.Get("b2").found);

  fs::remove_all(dir);
  std::cout << "TestBulk passed\n";
}

static void TestKeywordSearchAndBM25Ranking() {
  std::string dir = TmpDir("search");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  shard.Index("d1", R"({"title":"quick brown fox jumps","brand":"a","price":1})");
  shard.Index("d2", R"({"title":"quick quick quick fox","brand":"a","price":1})");
  shard.Index("d3", R"({"title":"completely unrelated content","brand":"a","price":1})");

  SearchParamsQ params;
  params.query_text = "quick fox";
  params.fields = {"title"};
  params.size = 10;
  auto res = shard.Search(params);

  assert(res.total_hits == 2);  // d3 doesn't match either term
  assert(res.hits.size() == 2);
  // d2 has higher term frequency for "quick" -> should rank above d1
  assert(res.hits[0].doc_id == "d2");
  assert(res.hits[1].doc_id == "d1");
  assert(res.hits[0].score > res.hits[1].score);

  fs::remove_all(dir);
  std::cout << "TestKeywordSearchAndBM25Ranking passed\n";
}

static void TestFiltersAndMatchAll() {
  std::string dir = TmpDir("filters");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  shard.Index("f1", R"({"title":"shoe one","brand":"Nike","price":50})");
  shard.Index("f2", R"({"title":"shoe two","brand":"Adidas","price":150})");
  shard.Index("f3", R"({"title":"shoe three","brand":"Nike","price":250})");

  // match_all
  SearchParamsQ all;
  all.size = 10;
  auto all_res = shard.Search(all);
  assert(all_res.total_hits == 3);

  // term filter on keyword field
  SearchParamsQ tf;
  tf.term_filters.push_back({"brand", "Nike"});
  tf.size = 10;
  auto tf_res = shard.Search(tf);
  assert(tf_res.total_hits == 2);

  // range filter on number field
  SearchParamsQ rf;
  RangeFilterQ range;
  range.field = "price";
  range.has_gte = true;
  range.gte = 100;
  rf.range_filters.push_back(range);
  rf.size = 10;
  auto rf_res = shard.Search(rf);
  assert(rf_res.total_hits == 2);  // f2 (150), f3 (250)

  // combined term + range
  SearchParamsQ combo;
  combo.term_filters.push_back({"brand", "nike"});  // filters normalize to lowercase
  RangeFilterQ range2;
  range2.field = "price";
  range2.has_gte = true;
  range2.gte = 100;
  combo.range_filters.push_back(range2);
  combo.size = 10;
  auto combo_res = shard.Search(combo);
  assert(combo_res.total_hits == 1);  // only f3: Nike AND price>=100

  fs::remove_all(dir);
  std::cout << "TestFiltersAndMatchAll passed\n";
}

static void TestPagination() {
  std::string dir = TmpDir("pagination");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());
  for (int i = 0; i < 5; i++) {
    shard.Index("p" + std::to_string(i),
                R"({"title":"item","brand":"a","price":)" + std::to_string(i) + "}");
  }
  SearchParamsQ p1;
  p1.size = 2;
  p1.from = 0;
  auto r1 = shard.Search(p1);
  assert(r1.total_hits == 5);
  assert(r1.hits.size() == 2);

  SearchParamsQ p2;
  p2.size = 2;
  p2.from = 4;
  auto r2 = shard.Search(p2);
  assert(r2.hits.size() == 1);  // only 1 left at offset 4 of 5

  fs::remove_all(dir);
  std::cout << "TestPagination passed\n";
}

static void TestAggregations() {
  std::string dir = TmpDir("agg");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  shard.Index("a1", R"({"title":"x","brand":"Nike","price":50})");
  shard.Index("a2", R"({"title":"x","brand":"Nike","price":150})");
  shard.Index("a3", R"({"title":"x","brand":"Adidas","price":80})");

  SearchParamsQ params;
  AggregationQ terms;
  terms.name = "by_brand";
  terms.type = "terms";
  terms.field = "brand";
  terms.size = 10;
  params.aggregations.push_back(terms);

  AggregationQ stats;
  stats.name = "price_stats";
  stats.type = "stats";
  stats.field = "price";
  params.aggregations.push_back(stats);

  auto res = shard.Search(params);
  assert(res.aggregations.size() == 2);

  const auto& brand_agg = res.aggregations[0];
  assert(brand_agg.name == "by_brand");
  bool found_nike = false;
  for (const auto& b : brand_agg.buckets) {
    if (b.key == "Nike") {
      found_nike = true;
      assert(b.doc_count == 2);
    }
  }
  assert(found_nike);

  const auto& price_agg = res.aggregations[1];
  assert(price_agg.stats.count == 3);
  assert(price_agg.stats.sum > 279 && price_agg.stats.sum < 281);  // 50+150+80=280

  fs::remove_all(dir);
  std::cout << "TestAggregations passed\n";
}

static void TestVectorAndHybridSearch() {
  std::string dir = TmpDir("vector");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  shard.Index("v1", R"({"title":"alpha","brand":"a","price":1,"embedding":[1,0,0,0]})");
  shard.Index("v2", R"({"title":"beta","brand":"a","price":1,"embedding":[0,1,0,0]})");
  shard.Index("v3", R"({"title":"gamma","brand":"a","price":1,"embedding":[0.9,0.1,0,0]})");

  SearchParamsQ params;
  params.has_vector = true;
  params.vector.field = "embedding";
  params.vector.embedding = {1, 0, 0, 0};
  params.vector.k = 3;
  params.vector_weight = 1.0;  // pure vector ranking (no keyword query)
  params.size = 3;
  auto res = shard.Search(params);
  assert(res.hits.size() == 3);
  // v1 (exact match direction) and v3 (close) should rank above v2 (orthogonal)
  assert(res.hits[0].doc_id == "v1");
  assert(res.hits[2].doc_id == "v2");

  fs::remove_all(dir);
  std::cout << "TestVectorAndHybridSearch passed\n";
}

static void TestFlushAndSearchAcrossSegments() {
  std::string dir = TmpDir("flush");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  shard.Index("s1", R"({"title":"quick fox","brand":"a","price":1})");
  auto flush_res = shard.Flush();
  assert(flush_res.success);
  assert(flush_res.segment_id == 0);

  auto status_after_flush = shard.Status();
  assert(status_after_flush.segment_count == 1);
  assert(status_after_flush.memtable_doc_count == 0);
  assert(status_after_flush.wal_size_bytes == 0);  // truncated after flush

  // index more into a fresh memtable, search should span segment + memtable
  shard.Index("s2", R"({"title":"quick dog","brand":"a","price":1})");

  SearchParamsQ params;
  params.query_text = "quick";
  params.fields = {"title"};
  params.size = 10;
  auto res = shard.Search(params);
  assert(res.total_hits == 2);  // s1 (segment) + s2 (memtable)

  // delete a segment-resident doc, confirm it disappears from search
  shard.Delete("s1");
  auto res2 = shard.Search(params);
  assert(res2.total_hits == 1);
  assert(res2.hits[0].doc_id == "s2");

  fs::remove_all(dir);
  std::cout << "TestFlushAndSearchAcrossSegments passed\n";
}

static void TestMergeReclaimsDeletedDocs() {
  std::string dir = TmpDir("merge");
  fs::remove_all(dir);
  Shard shard("s0", dir, ProductMappings());

  shard.Index("m1", R"({"title":"one","brand":"a","price":1})");
  shard.Flush();
  shard.Index("m2", R"({"title":"two","brand":"a","price":1})");
  shard.Flush();
  shard.Index("m3", R"({"title":"three","brand":"a","price":1})");
  shard.Flush();

  assert(shard.Status().segment_count == 3);

  shard.Delete("m2");
  auto merge_res = shard.Merge();
  assert(merge_res.success);
  assert(merge_res.segments_merged == 3);

  auto status = shard.Status();
  assert(status.segment_count == 1);
  assert(status.doc_count == 2);  // m1, m3 survive; m2 physically dropped

  assert(!shard.Get("m2").found);
  assert(shard.Get("m1").found);
  assert(shard.Get("m3").found);

  fs::remove_all(dir);
  std::cout << "TestMergeReclaimsDeletedDocs passed\n";
}

static void TestCrashRecoveryViaWALReplay() {
  std::string dir = TmpDir("recovery");
  fs::remove_all(dir);
  {
    Shard shard("s0", dir, ProductMappings());
    shard.Index("r1", R"({"title":"before flush","brand":"a","price":1})");
    shard.Flush();  // durable in a segment
    shard.Index("r2", R"({"title":"after flush not yet flushed","brand":"a","price":1})");
    // NOTE: no explicit Flush() for r2 — it only exists in the WAL + memtable.
    // Destructing `shard` here simulates a process exit/crash: no graceful
    // shutdown hook runs, we just rely on what was already durably persisted
    // (segment for r1, WAL record for r2).
  }

  {
    Shard shard("s0", dir, ProductMappings());  // "restart": re-opens segments + replays WAL
    auto g1 = shard.Get("r1");
    assert(g1.found);
    auto g2 = shard.Get("r2");
    assert(g2.found);  // recovered purely from WAL replay
    assert(g2.json.find("not yet flushed") != std::string::npos);

    auto status = shard.Status();
    assert(status.segment_count == 1);
    assert(status.memtable_doc_count == 1);  // r2 replayed back into the memtable
  }

  fs::remove_all(dir);
  std::cout << "TestCrashRecoveryViaWALReplay passed\n";
}

static void TestVectorIndexSurvivesRestart() {
  std::string dir = TmpDir("vecrestart");
  fs::remove_all(dir);
  {
    Shard shard("s0", dir, ProductMappings());
    shard.Index("vr1", R"({"title":"a","brand":"a","price":1,"embedding":[1,0,0,0]})");
    shard.Flush();
  }
  {
    Shard shard("s0", dir, ProductMappings());
    SearchParamsQ params;
    params.has_vector = true;
    params.vector.field = "embedding";
    params.vector.embedding = {1, 0, 0, 0};
    params.vector.k = 1;
    params.vector_weight = 1.0;
    params.size = 1;
    auto res = shard.Search(params);
    assert(res.hits.size() == 1);
    assert(res.hits[0].doc_id == "vr1");
  }
  fs::remove_all(dir);
  std::cout << "TestVectorIndexSurvivesRestart passed\n";
}

int main() {
  TestCRUD();
  TestBulk();
  TestKeywordSearchAndBM25Ranking();
  TestFiltersAndMatchAll();
  TestPagination();
  TestAggregations();
  TestVectorAndHybridSearch();
  TestFlushAndSearchAcrossSegments();
  TestMergeReclaimsDeletedDocs();
  TestCrashRecoveryViaWALReplay();
  TestVectorIndexSurvivesRestart();
  std::cout << "shard tests passed\n";
  return 0;
}
