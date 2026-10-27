#include "roaring_bitmap.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <vector>

using dse::bitmap::RoaringBitmap;

namespace {

void TestBasics() {
  RoaringBitmap bm;
  assert(bm.IsEmpty());
  assert(bm.Cardinality() == 0);

  std::vector<uint32_t> values = {5, 70000, 200000, 4294967295u};
  for (uint32_t v : values) bm.Add(v);

  assert(!bm.IsEmpty());
  assert(bm.Cardinality() == values.size());
  for (uint32_t v : values) assert(bm.Contains(v));
  assert(!bm.Contains(6));
  assert(!bm.Contains(70001));
  assert(!bm.Contains(0));

  std::vector<uint32_t> expected_sorted = {5, 70000, 200000, 4294967295u};
  assert(bm.ToVector() == expected_sorted);

  bm.Remove(70000);
  assert(bm.Cardinality() == 3);
  assert(!bm.Contains(70000));
  assert(bm.Contains(5));
  assert(bm.Contains(200000));
  assert(bm.Contains(4294967295u));

  // Remove a value that doesn't exist -- should be a no-op.
  bm.Remove(123456789);
  assert(bm.Cardinality() == 3);

  // Re-add and re-remove.
  bm.Add(70000);
  assert(bm.Contains(70000));
  bm.Remove(70000);
  assert(!bm.Contains(70000));

  std::cout << "TestBasics passed\n";
}

void TestCardinality() {
  RoaringBitmap bm;
  for (uint32_t i = 0; i < 1000; ++i) bm.Add(i * 37);
  assert(bm.Cardinality() == 1000);
  for (uint32_t i = 0; i < 500; ++i) bm.Remove(i * 37);
  assert(bm.Cardinality() == 500);
  // Removing duplicates / already-removed values shouldn't change cardinality.
  for (uint32_t i = 0; i < 500; ++i) bm.Remove(i * 37);
  assert(bm.Cardinality() == 500);
  std::cout << "TestCardinality passed\n";
}

void TestBitsetContainer() {
  RoaringBitmap bm;
  // Force one chunk (high key 0) into bitset representation by adding
  // more than 4096 distinct low values within the same 65536 range.
  const uint32_t kCount = 5000;
  for (uint32_t i = 0; i < kCount; ++i) {
    bm.Add(i);  // all within high-key 0, low values 0..4999
  }
  assert(bm.Cardinality() == kCount);
  for (uint32_t i = 0; i < kCount; ++i) assert(bm.Contains(i));
  assert(!bm.Contains(kCount));
  assert(!bm.Contains(70000));

  std::vector<uint32_t> vec = bm.ToVector();
  assert(vec.size() == kCount);
  for (size_t i = 0; i < vec.size(); ++i) assert(vec[i] == i);
  for (size_t i = 1; i < vec.size(); ++i) assert(vec[i - 1] < vec[i]);

  // Remove some values from the now-bitset chunk and re-check.
  bm.Remove(10);
  bm.Remove(4999);
  assert(!bm.Contains(10));
  assert(!bm.Contains(4999));
  assert(bm.Cardinality() == kCount - 2);

  std::cout << "TestBitsetContainer passed\n";
}

void TestSetOpsHandComputed() {
  RoaringBitmap a, b;
  for (uint32_t v : {1u, 2u, 3u, 70000u, 70001u}) a.Add(v);
  for (uint32_t v : {2u, 3u, 4u, 70001u, 80000u}) b.Add(v);

  RoaringBitmap and_result = RoaringBitmap::And(a, b);
  std::vector<uint32_t> expected_and = {2, 3, 70001};
  assert(and_result.ToVector() == expected_and);

  RoaringBitmap or_result = RoaringBitmap::Or(a, b);
  std::vector<uint32_t> expected_or = {1, 2, 3, 4, 70000, 70001, 80000};
  assert(or_result.ToVector() == expected_or);

  RoaringBitmap andnot_result = RoaringBitmap::AndNot(a, b);
  std::vector<uint32_t> expected_andnot = {1, 70000};
  assert(andnot_result.ToVector() == expected_andnot);

  // Empty bitmap edge cases.
  RoaringBitmap empty;
  assert(RoaringBitmap::And(a, empty).IsEmpty());
  assert(RoaringBitmap::And(empty, a).IsEmpty());
  assert(RoaringBitmap::Or(a, empty).ToVector() == a.ToVector());
  assert(RoaringBitmap::Or(empty, a).ToVector() == a.ToVector());
  assert(RoaringBitmap::AndNot(a, empty).ToVector() == a.ToVector());
  assert(RoaringBitmap::AndNot(empty, a).IsEmpty());

  // Non-overlapping chunk keys.
  RoaringBitmap c, d;
  c.Add(5);
  d.Add(70000);
  assert(RoaringBitmap::And(c, d).IsEmpty());
  std::vector<uint32_t> expected_cd_or = {5, 70000};
  assert(RoaringBitmap::Or(c, d).ToVector() == expected_cd_or);
  assert(RoaringBitmap::AndNot(c, d).ToVector() == std::vector<uint32_t>{5});

  std::cout << "TestSetOpsHandComputed passed\n";
}

void TestSetOpsRandomized() {
  std::mt19937 rng(42);  // fixed seed for determinism
  std::uniform_int_distribution<uint32_t> value_dist(0, 300000);

  std::set<uint32_t> ref_a, ref_b;
  RoaringBitmap a, b;

  const int kNumOps = 6000;
  for (int i = 0; i < kNumOps; ++i) {
    uint32_t v = value_dist(rng);
    if (i % 2 == 0) {
      ref_a.insert(v);
      a.Add(v);
    } else {
      ref_b.insert(v);
      b.Add(v);
    }
  }

  // Sprinkle in some removals too.
  for (int i = 0; i < 500; ++i) {
    uint32_t v = value_dist(rng);
    ref_a.erase(v);
    a.Remove(v);
  }
  for (int i = 0; i < 500; ++i) {
    uint32_t v = value_dist(rng);
    ref_b.erase(v);
    b.Remove(v);
  }

  // Verify basic Contains/ToVector against reference sets first.
  {
    std::vector<uint32_t> a_vec = a.ToVector();
    std::vector<uint32_t> ref_a_vec(ref_a.begin(), ref_a.end());
    assert(a_vec == ref_a_vec);
    assert(a.Cardinality() == ref_a.size());
  }

  std::set<uint32_t> ref_and, ref_or, ref_andnot;
  for (uint32_t v : ref_a) {
    if (ref_b.count(v)) ref_and.insert(v);
    if (!ref_b.count(v)) ref_andnot.insert(v);
  }
  ref_or = ref_a;
  for (uint32_t v : ref_b) ref_or.insert(v);

  RoaringBitmap and_result = RoaringBitmap::And(a, b);
  RoaringBitmap or_result = RoaringBitmap::Or(a, b);
  RoaringBitmap andnot_result = RoaringBitmap::AndNot(a, b);

  std::vector<uint32_t> ref_and_vec(ref_and.begin(), ref_and.end());
  std::vector<uint32_t> ref_or_vec(ref_or.begin(), ref_or.end());
  std::vector<uint32_t> ref_andnot_vec(ref_andnot.begin(), ref_andnot.end());

  assert(and_result.ToVector() == ref_and_vec);
  assert(or_result.ToVector() == ref_or_vec);
  assert(andnot_result.ToVector() == ref_andnot_vec);

  std::cout << "TestSetOpsRandomized passed\n";
}

void TestSerializationRoundTrip() {
  // Empty bitmap.
  {
    RoaringBitmap bm;
    std::string bytes = bm.Serialize();
    RoaringBitmap round_tripped = RoaringBitmap::Deserialize(bytes);
    assert(round_tripped.IsEmpty());
    assert(round_tripped.ToVector() == bm.ToVector());
  }

  // Small array-only bitmap spanning multiple chunks.
  {
    RoaringBitmap bm;
    for (uint32_t v : {5u, 6u, 70000u, 200000u, 4294967295u}) bm.Add(v);
    std::string bytes = bm.Serialize();
    RoaringBitmap round_tripped = RoaringBitmap::Deserialize(bytes);
    assert(round_tripped.ToVector() == bm.ToVector());
    assert(round_tripped.Cardinality() == bm.Cardinality());
  }

  // Bitmap containing a bitset-representation chunk plus an array chunk.
  {
    RoaringBitmap bm;
    for (uint32_t i = 0; i < 5000; ++i) bm.Add(i);          // forces bitset chunk (key 0)
    for (uint32_t v : {70000u, 70005u, 80000u}) bm.Add(v);  // array chunk (key 1)
    std::string bytes = bm.Serialize();
    RoaringBitmap round_tripped = RoaringBitmap::Deserialize(bytes);
    assert(round_tripped.ToVector() == bm.ToVector());
    assert(round_tripped.Cardinality() == bm.Cardinality());
    for (uint32_t i = 0; i < 5000; ++i) assert(round_tripped.Contains(i));
    for (uint32_t v : {70000u, 70005u, 80000u}) assert(round_tripped.Contains(v));
  }

  std::cout << "TestSerializationRoundTrip passed\n";
}

}  // namespace

int main() {
  TestBasics();
  TestCardinality();
  TestBitsetContainer();
  TestSetOpsHandComputed();
  TestSetOpsRandomized();
  TestSerializationRoundTrip();

  std::cout << "roaring bitmap tests passed\n";
  return 0;
}
