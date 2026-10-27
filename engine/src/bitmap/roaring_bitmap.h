// roaring_bitmap.h
//
// A simplified Roaring Bitmap: a compressed sorted set of uint32_t values
// used to represent posting lists (document IDs matching a term) and to
// perform fast set intersection (AND), union (OR), and difference (AND NOT)
// operations chunk-by-chunk instead of naive full scans.
//
// Design:
//  - Each uint32_t value is split into a 16-bit "high" key (value >> 16)
//    and a 16-bit "low" value (value & 0xFFFF).
//  - Values are grouped by high key into a std::map<uint16_t, Container>,
//    which keeps chunks sorted for ordered iteration/serialization.
//  - Each Container represents up to 65536 values sharing the same high
//    key, and is internally either:
//      * ArrayContainer: a sorted std::vector<uint16_t>, used while the
//        chunk has <= kArrayMaxCardinality values (sparse chunk).
//      * BitsetContainer: a fixed std::array<uint64_t, 1024> (65536 bits),
//        used once the chunk exceeds kArrayMaxCardinality values (dense
//        chunk).
//  - Conversion from array -> bitset happens automatically inside Add()
//    once the threshold is crossed. Conversion from bitset -> array on
//    Remove() is intentionally NOT implemented (documented simplification
//    below) -- once a chunk becomes dense it stays a bitset container even
//    if values are later removed from it. This trades a small amount of
//    memory for simplicity; correctness is unaffected.

#ifndef DSE_BITMAP_ROARING_BITMAP_H_
#define DSE_BITMAP_ROARING_BITMAP_H_

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace dse::bitmap {

class RoaringBitmap {
 public:
  RoaringBitmap() = default;

  void Add(uint32_t value);
  void Remove(uint32_t value);
  bool Contains(uint32_t value) const;
  uint64_t Cardinality() const;             // total number of set values across all chunks
  std::vector<uint32_t> ToVector() const;   // ascending sorted output
  bool IsEmpty() const;

  static RoaringBitmap And(const RoaringBitmap& a, const RoaringBitmap& b);     // intersection
  static RoaringBitmap Or(const RoaringBitmap& a, const RoaringBitmap& b);      // union
  static RoaringBitmap AndNot(const RoaringBitmap& a, const RoaringBitmap& b);  // a minus b

  // Binary serialization for on-disk segment storage. Byte layout (all
  // integers little-endian):
  //
  //   [4 bytes]  magic "RBM1"
  //   [4 bytes]  uint32 num_chunks
  //   repeated num_chunks times:
  //     [2 bytes]  uint16 high_key
  //     [1 byte]   uint8 container_type   (0 = array, 1 = bitset)
  //     if array:
  //       [4 bytes]  uint32 count
  //       [count * 2 bytes]  uint16 low values, in ascending sorted order
  //     if bitset:
  //       [1024 * 8 bytes]  1024 uint64 words, 65536 bits total
  //
  // This format is fully self-describing: reading it back reconstructs an
  // identical set of values (though a bitset chunk with few members that
  // was written on disk this way will read back as a bitset container,
  // matching the in-memory simplification documented above).
  std::string Serialize() const;
  static RoaringBitmap Deserialize(const std::string& bytes);

 private:
  static constexpr size_t kArrayMaxCardinality = 4096;
  static constexpr size_t kBitsetWords = 1024;  // 1024 * 64 = 65536 bits

  struct ArrayContainer {
    std::vector<uint16_t> values;  // kept sorted, unique
  };

  struct BitsetContainer {
    std::array<uint64_t, kBitsetWords> words{};
    uint32_t count = 0;  // cached cardinality
  };

  using Container = std::variant<ArrayContainer, BitsetContainer>;

  static bool ContainerContains(const Container& c, uint16_t low);
  static void ContainerAdd(Container& c, uint16_t low);
  static void ContainerRemove(Container& c, uint16_t low);
  static uint64_t ContainerCardinality(const Container& c);
  static std::vector<uint16_t> ContainerToSortedVector(const Container& c);

  static Container ContainerAnd(const Container& a, const Container& b);
  static Container ContainerOr(const Container& a, const Container& b);
  static Container ContainerAndNot(const Container& a, const Container& b);

  std::map<uint16_t, Container> chunks_;
};

}  // namespace dse::bitmap

#endif  // DSE_BITMAP_ROARING_BITMAP_H_
