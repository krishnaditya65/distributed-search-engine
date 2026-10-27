#include "roaring_bitmap.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dse::bitmap {

namespace {

inline uint16_t HighKey(uint32_t value) { return static_cast<uint16_t>(value >> 16); }
inline uint16_t LowValue(uint32_t value) { return static_cast<uint16_t>(value & 0xFFFFu); }

void AppendU32(std::string* out, uint32_t v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  out->append(buf, 4);
}

void AppendU16(std::string* out, uint16_t v) {
  char buf[2];
  std::memcpy(buf, &v, 2);
  out->append(buf, 2);
}

void AppendU8(std::string* out, uint8_t v) { out->push_back(static_cast<char>(v)); }

uint32_t ReadU32(const std::string& s, size_t pos) {
  uint32_t v;
  std::memcpy(&v, s.data() + pos, 4);
  return v;
}

uint16_t ReadU16(const std::string& s, size_t pos) {
  uint16_t v;
  std::memcpy(&v, s.data() + pos, 2);
  return v;
}

uint8_t ReadU8(const std::string& s, size_t pos) { return static_cast<uint8_t>(s[pos]); }

}  // namespace

// ---------- Container helpers ----------

bool RoaringBitmap::ContainerContains(const Container& c, uint16_t low) {
  if (std::holds_alternative<ArrayContainer>(c)) {
    const auto& arr = std::get<ArrayContainer>(c).values;
    return std::binary_search(arr.begin(), arr.end(), low);
  } else {
    const auto& bitset = std::get<BitsetContainer>(c);
    return (bitset.words[low >> 6] >> (low & 63)) & 1ULL;
  }
}

void RoaringBitmap::ContainerAdd(Container& c, uint16_t low) {
  if (std::holds_alternative<ArrayContainer>(c)) {
    auto& arr = std::get<ArrayContainer>(c).values;
    auto it = std::lower_bound(arr.begin(), arr.end(), low);
    if (it != arr.end() && *it == low) return;  // already present
    if (arr.size() + 1 > kArrayMaxCardinality) {
      // Convert to bitset container.
      BitsetContainer bitset;
      for (uint16_t v : arr) {
        bitset.words[v >> 6] |= (1ULL << (v & 63));
      }
      bitset.words[low >> 6] |= (1ULL << (low & 63));
      bitset.count = static_cast<uint32_t>(arr.size() + 1);
      c = std::move(bitset);
    } else {
      arr.insert(it, low);
    }
  } else {
    auto& bitset = std::get<BitsetContainer>(c);
    uint64_t& word = bitset.words[low >> 6];
    uint64_t bit = 1ULL << (low & 63);
    if (!(word & bit)) {
      word |= bit;
      bitset.count++;
    }
  }
}

void RoaringBitmap::ContainerRemove(Container& c, uint16_t low) {
  // Note: bitset containers are never converted back to array containers on
  // removal (documented simplification in roaring_bitmap.h).
  if (std::holds_alternative<ArrayContainer>(c)) {
    auto& arr = std::get<ArrayContainer>(c).values;
    auto it = std::lower_bound(arr.begin(), arr.end(), low);
    if (it != arr.end() && *it == low) arr.erase(it);
  } else {
    auto& bitset = std::get<BitsetContainer>(c);
    uint64_t& word = bitset.words[low >> 6];
    uint64_t bit = 1ULL << (low & 63);
    if (word & bit) {
      word &= ~bit;
      bitset.count--;
    }
  }
}

uint64_t RoaringBitmap::ContainerCardinality(const Container& c) {
  if (std::holds_alternative<ArrayContainer>(c)) {
    return std::get<ArrayContainer>(c).values.size();
  } else {
    return std::get<BitsetContainer>(c).count;
  }
}

std::vector<uint16_t> RoaringBitmap::ContainerToSortedVector(const Container& c) {
  if (std::holds_alternative<ArrayContainer>(c)) {
    return std::get<ArrayContainer>(c).values;
  } else {
    const auto& bitset = std::get<BitsetContainer>(c);
    std::vector<uint16_t> out;
    out.reserve(bitset.count);
    for (size_t w = 0; w < kBitsetWords; ++w) {
      uint64_t word = bitset.words[w];
      while (word != 0) {
        int bit = __builtin_ctzll(word);
        out.push_back(static_cast<uint16_t>(w * 64 + bit));
        word &= word - 1;
      }
    }
    return out;
  }
}

RoaringBitmap::Container RoaringBitmap::ContainerAnd(const Container& a, const Container& b) {
  // Operate on sorted-value views regardless of underlying representation;
  // result starts as an array and may be promoted by ContainerAdd if needed
  // (in practice an intersection is never larger than either input, so no
  // promotion will occur beyond what the inputs already required, but we
  // reuse ContainerAdd for correctness/simplicity).
  std::vector<uint16_t> av = ContainerToSortedVector(a);
  std::vector<uint16_t> bv = ContainerToSortedVector(b);
  Container result = ArrayContainer{};
  size_t i = 0, j = 0;
  while (i < av.size() && j < bv.size()) {
    if (av[i] == bv[j]) {
      ContainerAdd(result, av[i]);
      ++i;
      ++j;
    } else if (av[i] < bv[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return result;
}

RoaringBitmap::Container RoaringBitmap::ContainerOr(const Container& a, const Container& b) {
  std::vector<uint16_t> av = ContainerToSortedVector(a);
  std::vector<uint16_t> bv = ContainerToSortedVector(b);
  Container result = ArrayContainer{};
  size_t i = 0, j = 0;
  while (i < av.size() || j < bv.size()) {
    if (i < av.size() && (j >= bv.size() || av[i] < bv[j])) {
      ContainerAdd(result, av[i]);
      ++i;
    } else if (j < bv.size() && (i >= av.size() || bv[j] < av[i])) {
      ContainerAdd(result, bv[j]);
      ++j;
    } else {
      ContainerAdd(result, av[i]);
      ++i;
      ++j;
    }
  }
  return result;
}

RoaringBitmap::Container RoaringBitmap::ContainerAndNot(const Container& a, const Container& b) {
  std::vector<uint16_t> av = ContainerToSortedVector(a);
  std::vector<uint16_t> bv = ContainerToSortedVector(b);
  Container result = ArrayContainer{};
  size_t i = 0, j = 0;
  while (i < av.size()) {
    while (j < bv.size() && bv[j] < av[i]) ++j;
    if (j < bv.size() && bv[j] == av[i]) {
      ++i;
    } else {
      ContainerAdd(result, av[i]);
      ++i;
    }
  }
  return result;
}

// ---------- RoaringBitmap public API ----------

void RoaringBitmap::Add(uint32_t value) {
  uint16_t high = HighKey(value);
  uint16_t low = LowValue(value);
  auto it = chunks_.find(high);
  if (it == chunks_.end()) {
    it = chunks_.emplace(high, Container{ArrayContainer{}}).first;
  }
  ContainerAdd(it->second, low);
}

void RoaringBitmap::Remove(uint32_t value) {
  uint16_t high = HighKey(value);
  auto it = chunks_.find(high);
  if (it == chunks_.end()) return;
  ContainerRemove(it->second, LowValue(value));
  if (ContainerCardinality(it->second) == 0) {
    chunks_.erase(it);
  }
}

bool RoaringBitmap::Contains(uint32_t value) const {
  auto it = chunks_.find(HighKey(value));
  if (it == chunks_.end()) return false;
  return ContainerContains(it->second, LowValue(value));
}

uint64_t RoaringBitmap::Cardinality() const {
  uint64_t total = 0;
  for (const auto& [key, container] : chunks_) {
    total += ContainerCardinality(container);
  }
  return total;
}

std::vector<uint32_t> RoaringBitmap::ToVector() const {
  std::vector<uint32_t> out;
  out.reserve(Cardinality());
  for (const auto& [key, container] : chunks_) {
    std::vector<uint16_t> lows = ContainerToSortedVector(container);
    for (uint16_t low : lows) {
      out.push_back((static_cast<uint32_t>(key) << 16) | low);
    }
  }
  return out;
}

bool RoaringBitmap::IsEmpty() const { return chunks_.empty(); }

RoaringBitmap RoaringBitmap::And(const RoaringBitmap& a, const RoaringBitmap& b) {
  RoaringBitmap result;
  auto it_a = a.chunks_.begin();
  auto it_b = b.chunks_.begin();
  while (it_a != a.chunks_.end() && it_b != b.chunks_.end()) {
    if (it_a->first == it_b->first) {
      Container c = ContainerAnd(it_a->second, it_b->second);
      if (ContainerCardinality(c) > 0) {
        result.chunks_.emplace(it_a->first, std::move(c));
      }
      ++it_a;
      ++it_b;
    } else if (it_a->first < it_b->first) {
      ++it_a;
    } else {
      ++it_b;
    }
  }
  return result;
}

RoaringBitmap RoaringBitmap::Or(const RoaringBitmap& a, const RoaringBitmap& b) {
  RoaringBitmap result;
  auto it_a = a.chunks_.begin();
  auto it_b = b.chunks_.begin();
  while (it_a != a.chunks_.end() || it_b != b.chunks_.end()) {
    if (it_a != a.chunks_.end() && (it_b == b.chunks_.end() || it_a->first < it_b->first)) {
      result.chunks_.emplace(it_a->first, it_a->second);
      ++it_a;
    } else if (it_b != b.chunks_.end() && (it_a == a.chunks_.end() || it_b->first < it_a->first)) {
      result.chunks_.emplace(it_b->first, it_b->second);
      ++it_b;
    } else {
      Container c = ContainerOr(it_a->second, it_b->second);
      result.chunks_.emplace(it_a->first, std::move(c));
      ++it_a;
      ++it_b;
    }
  }
  return result;
}

RoaringBitmap RoaringBitmap::AndNot(const RoaringBitmap& a, const RoaringBitmap& b) {
  RoaringBitmap result;
  auto it_a = a.chunks_.begin();
  auto it_b = b.chunks_.begin();
  while (it_a != a.chunks_.end()) {
    if (it_b == b.chunks_.end() || it_a->first < it_b->first) {
      result.chunks_.emplace(it_a->first, it_a->second);
      ++it_a;
    } else if (it_a->first > it_b->first) {
      ++it_b;
    } else {
      Container c = ContainerAndNot(it_a->second, it_b->second);
      if (ContainerCardinality(c) > 0) {
        result.chunks_.emplace(it_a->first, std::move(c));
      }
      ++it_a;
      ++it_b;
    }
  }
  return result;
}

std::string RoaringBitmap::Serialize() const {
  std::string out;
  out.append("RBM1", 4);
  AppendU32(&out, static_cast<uint32_t>(chunks_.size()));
  for (const auto& [key, container] : chunks_) {
    AppendU16(&out, key);
    if (std::holds_alternative<ArrayContainer>(container)) {
      AppendU8(&out, 0);
      const auto& values = std::get<ArrayContainer>(container).values;
      AppendU32(&out, static_cast<uint32_t>(values.size()));
      for (uint16_t v : values) AppendU16(&out, v);
    } else {
      AppendU8(&out, 1);
      const auto& bitset = std::get<BitsetContainer>(container);
      for (uint64_t word : bitset.words) {
        char buf[8];
        std::memcpy(buf, &word, 8);
        out.append(buf, 8);
      }
    }
  }
  return out;
}

RoaringBitmap RoaringBitmap::Deserialize(const std::string& bytes) {
  RoaringBitmap result;
  if (bytes.size() < 8 || bytes.compare(0, 4, "RBM1") != 0) {
    throw std::runtime_error("RoaringBitmap::Deserialize: invalid magic/header");
  }
  size_t pos = 4;
  uint32_t num_chunks = ReadU32(bytes, pos);
  pos += 4;
  for (uint32_t i = 0; i < num_chunks; ++i) {
    if (pos + 3 > bytes.size()) throw std::runtime_error("RoaringBitmap::Deserialize: truncated chunk header");
    uint16_t key = ReadU16(bytes, pos);
    pos += 2;
    uint8_t type = ReadU8(bytes, pos);
    pos += 1;
    if (type == 0) {
      if (pos + 4 > bytes.size()) throw std::runtime_error("RoaringBitmap::Deserialize: truncated array count");
      uint32_t count = ReadU32(bytes, pos);
      pos += 4;
      ArrayContainer arr;
      arr.values.reserve(count);
      for (uint32_t k = 0; k < count; ++k) {
        if (pos + 2 > bytes.size()) throw std::runtime_error("RoaringBitmap::Deserialize: truncated array values");
        arr.values.push_back(ReadU16(bytes, pos));
        pos += 2;
      }
      result.chunks_.emplace(key, Container{std::move(arr)});
    } else if (type == 1) {
      BitsetContainer bitset;
      if (pos + kBitsetWords * 8 > bytes.size()) {
        throw std::runtime_error("RoaringBitmap::Deserialize: truncated bitset words");
      }
      uint32_t count = 0;
      for (size_t w = 0; w < kBitsetWords; ++w) {
        uint64_t word;
        std::memcpy(&word, bytes.data() + pos, 8);
        pos += 8;
        bitset.words[w] = word;
        count += static_cast<uint32_t>(__builtin_popcountll(word));
      }
      bitset.count = count;
      result.chunks_.emplace(key, Container{std::move(bitset)});
    } else {
      throw std::runtime_error("RoaringBitmap::Deserialize: unknown container type");
    }
  }
  return result;
}

}  // namespace dse::bitmap
