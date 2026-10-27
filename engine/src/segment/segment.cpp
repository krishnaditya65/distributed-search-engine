#include "segment/segment.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace dse::segment {

namespace {

// ---------------------------------------------------------------------------
// On-disk format (all multi-byte integers little-endian, native to arm64/x86):
//
//   magic:            char[5]  "DSEG1"
//   field_count:      uint32
//     for each field: field_len:uint32, field_bytes, avg_len:double
//   term_count:       uint32
//     for each (field,term):
//       field_len:uint32, field_bytes
//       term_len:uint32,  term_bytes
//       bitmap_len:uint32, bitmap_bytes         (RoaringBitmap::Serialize())
//       freq_count:uint32
//       freq_count * uint32                      (term_freqs, aligned with
//                                                  bitmap.ToVector() order)
//   doclen_count:     uint32
//     for each: field_len:uint32, field_bytes, doc_id:uint32, length:uint32
//   doc_count:        uint32
//     for each doc:
//       internal_id:uint32
//       doc_id_len:uint32, doc_id_bytes
//       source_len:uint32, source_bytes
//
// This is a plain sequential binary format, written once via std::ofstream
// (simplest way to get exact bytes on disk) and then re-opened read-only via
// mmap for all subsequent reads.
// ---------------------------------------------------------------------------

void WriteU32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void WriteU64Double(std::ofstream& out, double v) { out.write(reinterpret_cast<const char*>(&v), 8); }
void WriteString(std::ofstream& out, const std::string& s) {
  WriteU32(out, static_cast<uint32_t>(s.size()));
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// A small cursor over the mmap'd bytes for sequential parsing.
struct Reader {
  const uint8_t* data;
  size_t size;
  size_t pos = 0;

  void CheckAvail(size_t n) const {
    if (pos + n > size) throw std::runtime_error("segment file truncated/corrupt");
  }
  uint32_t ReadU32() {
    CheckAvail(4);
    uint32_t v;
    std::memcpy(&v, data + pos, 4);
    pos += 4;
    return v;
  }
  double ReadDouble() {
    CheckAvail(8);
    double v;
    std::memcpy(&v, data + pos, 8);
    pos += 8;
    return v;
  }
  std::string ReadString() {
    uint32_t len = ReadU32();
    CheckAvail(len);
    std::string s(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    return s;
  }
  std::string ReadBytes(uint32_t len) {
    CheckAvail(len);
    std::string s(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    return s;
  }
};

}  // namespace

void Segment::Write(const std::string& path, const SegmentBuildInput& input) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("failed to open segment file for write: " + path);

  out.write("DSEG1", 5);

  WriteU32(out, static_cast<uint32_t>(input.fields.size()));
  for (const auto& field : input.fields) {
    WriteString(out, field);
    double avg = 0.0;
    auto it = input.avg_field_length.find(field);
    if (it != input.avg_field_length.end()) avg = it->second;
    WriteU64Double(out, avg);
  }

  uint32_t term_count = 0;
  for (const auto& [field, terms] : input.postings) term_count += static_cast<uint32_t>(terms.size());
  WriteU32(out, term_count);
  for (const auto& [field, terms] : input.postings) {
    for (const auto& [term, postings] : terms) {
      WriteString(out, field);
      WriteString(out, term);
      std::string bitmap_bytes = postings.doc_ids.Serialize();
      WriteU32(out, static_cast<uint32_t>(bitmap_bytes.size()));
      out.write(bitmap_bytes.data(), static_cast<std::streamsize>(bitmap_bytes.size()));
      WriteU32(out, static_cast<uint32_t>(postings.term_freqs.size()));
      for (uint32_t f : postings.term_freqs) WriteU32(out, f);
    }
  }

  uint32_t doclen_count = 0;
  for (const auto& [field, m] : input.doc_field_lengths) doclen_count += static_cast<uint32_t>(m.size());
  WriteU32(out, doclen_count);
  for (const auto& [field, m] : input.doc_field_lengths) {
    for (const auto& [doc_id, length] : m) {
      WriteString(out, field);
      WriteU32(out, doc_id);
      WriteU32(out, length);
    }
  }

  WriteU32(out, static_cast<uint32_t>(input.docs.size()));
  for (const auto& d : input.docs) {
    WriteU32(out, d.internal_id);
    WriteString(out, d.doc_id);
    WriteString(out, d.source_json);
  }

  out.flush();
  if (!out) throw std::runtime_error("failed writing segment file: " + path);
}

std::unique_ptr<Segment> Segment::Open(const std::string& path) {
  auto seg = std::unique_ptr<Segment>(new Segment());
  seg->path_ = path;
  seg->mmap_ = std::make_unique<dse::storage::MMapFile>(dse::storage::MMapFile::OpenReadOnly(path));

  Reader r{seg->mmap_->Data(), seg->mmap_->Size(), 0};
  std::string magic = r.ReadBytes(5);
  if (magic != "DSEG1") throw std::runtime_error("bad segment magic in " + path);

  uint32_t field_count = r.ReadU32();
  for (uint32_t i = 0; i < field_count; i++) {
    std::string field = r.ReadString();
    double avg = r.ReadDouble();
    seg->avg_field_length_[field] = avg;
  }

  uint32_t term_count = r.ReadU32();
  for (uint32_t i = 0; i < term_count; i++) {
    std::string field = r.ReadString();
    std::string term = r.ReadString();
    uint32_t bitmap_len = r.ReadU32();
    std::string bitmap_bytes = r.ReadBytes(bitmap_len);
    dse::index::Postings postings;
    postings.doc_ids = dse::bitmap::RoaringBitmap::Deserialize(bitmap_bytes);
    uint32_t freq_count = r.ReadU32();
    postings.term_freqs.reserve(freq_count);
    for (uint32_t j = 0; j < freq_count; j++) postings.term_freqs.push_back(r.ReadU32());
    seg->postings_[field][term] = std::move(postings);
  }

  uint32_t doclen_count = r.ReadU32();
  for (uint32_t i = 0; i < doclen_count; i++) {
    std::string field = r.ReadString();
    uint32_t doc_id = r.ReadU32();
    uint32_t length = r.ReadU32();
    seg->doc_field_lengths_[field][doc_id] = length;
  }

  uint32_t doc_count = r.ReadU32();
  seg->doc_count_ = doc_count;
  for (uint32_t i = 0; i < doc_count; i++) {
    uint32_t internal_id = r.ReadU32();
    std::string doc_id = r.ReadString();
    std::string source = r.ReadString();
    seg->id_to_docid_[internal_id] = doc_id;
    seg->docid_to_id_[doc_id] = internal_id;
    seg->stored_source_[internal_id] = std::move(source);
  }

  return seg;
}

const dse::index::Postings* Segment::GetPostings(const std::string& field,
                                                  const std::string& term) const {
  auto fit = postings_.find(field);
  if (fit == postings_.end()) return nullptr;
  auto tit = fit->second.find(term);
  if (tit == fit->second.end()) return nullptr;
  return &tit->second;
}

uint32_t Segment::DocFieldLength(uint32_t doc_id, const std::string& field) const {
  auto fit = doc_field_lengths_.find(field);
  if (fit == doc_field_lengths_.end()) return 0;
  auto dit = fit->second.find(doc_id);
  return dit == fit->second.end() ? 0 : dit->second;
}

double Segment::AvgFieldLength(const std::string& field) const {
  auto it = avg_field_length_.find(field);
  return it == avg_field_length_.end() ? 0.0 : it->second;
}

uint64_t Segment::DocFreq(const std::string& field, const std::string& term) const {
  const auto* postings = GetPostings(field, term);
  return postings == nullptr ? 0 : postings->doc_ids.Cardinality();
}

uint32_t Segment::FieldDocCount(const std::string& field) const {
  auto it = doc_field_lengths_.find(field);
  return it == doc_field_lengths_.end() ? 0 : static_cast<uint32_t>(it->second.size());
}

void Segment::ForEachPosting(
    const std::function<void(const std::string&, const std::string&, const dse::index::Postings&)>&
        visitor) const {
  for (const auto& [field, terms] : postings_) {
    for (const auto& [term, postings] : terms) {
      visitor(field, term, postings);
    }
  }
}

std::vector<std::string> Segment::Fields() const {
  std::vector<std::string> out;
  out.reserve(postings_.size());
  for (const auto& [field, _] : postings_) out.push_back(field);
  return out;
}

std::optional<std::string> Segment::GetSource(uint32_t internal_id) const {
  auto it = stored_source_.find(internal_id);
  if (it == stored_source_.end()) return std::nullopt;
  return it->second;
}

std::optional<uint32_t> Segment::ResolveDocID(const std::string& doc_id) const {
  auto it = docid_to_id_.find(doc_id);
  if (it == docid_to_id_.end()) return std::nullopt;
  return it->second;
}

std::optional<std::string> Segment::ResolveInternalID(uint32_t internal_id) const {
  auto it = id_to_docid_.find(internal_id);
  if (it == id_to_docid_.end()) return std::nullopt;
  return it->second;
}

std::vector<uint32_t> Segment::AllDocIDs() const {
  std::vector<uint32_t> out;
  out.reserve(id_to_docid_.size());
  for (const auto& [id, _] : id_to_docid_) out.push_back(id);
  return out;
}

}  // namespace dse::segment
