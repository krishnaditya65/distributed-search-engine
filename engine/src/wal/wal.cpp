#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace dse::wal {

namespace {

// Table-based CRC32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
class Crc32Table {
 public:
  Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        if (c & 1u) {
          c = 0xEDB88320u ^ (c >> 1);
        } else {
          c = c >> 1;
        }
      }
      table_[i] = c;
    }
  }

  uint32_t table_[256];
};

const Crc32Table kCrcTable;

uint32_t Crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    uint8_t idx = static_cast<uint8_t>((crc ^ data[i]) & 0xFFu);
    crc = kCrcTable.table_[idx] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void AppendLE32(std::string* buf, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    buf->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

uint32_t ReadLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Reads exactly `len` bytes from fd into `out`, starting at the fd's current
// offset. Returns true if all `len` bytes were read, false if EOF was hit
// before `len` bytes could be gathered (i.e. a torn/short read).
bool ReadExact(int fd, void* out, size_t len) {
  uint8_t* p = static_cast<uint8_t*>(out);
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(fd, p + total, len - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) {
      // EOF before we got everything we wanted.
      return false;
    }
    total += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

WriteAheadLog::WriteAheadLog(const std::string& path) : path_(path) {
  fd_ = open(path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                             "WriteAheadLog: failed to open " + path_);
  }
}

WriteAheadLog::~WriteAheadLog() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

void WriteAheadLog::Append(const WalRecord& record) {
  std::string buf;
  buf.reserve(4 + record.doc_id.size() + 1 + 4 + record.json.size());

  AppendLE32(&buf, static_cast<uint32_t>(record.doc_id.size()));
  buf.append(record.doc_id);
  buf.push_back(static_cast<char>(static_cast<uint8_t>(record.type)));

  const std::string& json =
      record.type == WalOpType::kDelete ? std::string() : record.json;
  AppendLE32(&buf, static_cast<uint32_t>(json.size()));
  buf.append(json);

  uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(buf.data()),
                        buf.size());
  AppendLE32(&buf, crc);

  const char* cursor = buf.data();
  size_t remaining = buf.size();
  while (remaining > 0) {
    ssize_t n = write(fd_, cursor, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(),
                               "WriteAheadLog: write failed for " + path_);
    }
    cursor += n;
    remaining -= static_cast<size_t>(n);
  }

  if (fsync(fd_) != 0) {
    throw std::system_error(errno, std::generic_category(),
                             "WriteAheadLog: fsync failed for " + path_);
  }
}

std::vector<WalRecord> WriteAheadLog::ReadAll() const {
  std::vector<WalRecord> records;

  int fd = open(path_.c_str(), O_RDONLY);
  if (fd < 0) {
    return records;
  }

  while (true) {
    uint8_t len_buf[4];
    if (!ReadExact(fd, len_buf, 4)) break;
    uint32_t doc_id_len = ReadLE32(len_buf);

    std::string doc_id(doc_id_len, '\0');
    if (doc_id_len > 0 &&
        !ReadExact(fd, doc_id.data(), doc_id_len)) {
      break;
    }

    uint8_t type_byte;
    if (!ReadExact(fd, &type_byte, 1)) break;
    if (type_byte != static_cast<uint8_t>(WalOpType::kIndex) &&
        type_byte != static_cast<uint8_t>(WalOpType::kDelete)) {
      break;
    }

    uint8_t json_len_buf[4];
    if (!ReadExact(fd, json_len_buf, 4)) break;
    uint32_t json_len = ReadLE32(json_len_buf);

    std::string json(json_len, '\0');
    if (json_len > 0 && !ReadExact(fd, json.data(), json_len)) {
      break;
    }

    uint8_t crc_buf[4];
    if (!ReadExact(fd, crc_buf, 4)) break;
    uint32_t stored_crc = ReadLE32(crc_buf);

    // Recompute CRC over the same byte range that was written: doc_id_len,
    // doc_id, type, json_len, json.
    std::string frame;
    frame.reserve(4 + doc_id_len + 1 + 4 + json_len);
    AppendLE32(&frame, doc_id_len);
    frame.append(doc_id);
    frame.push_back(static_cast<char>(type_byte));
    AppendLE32(&frame, json_len);
    frame.append(json);

    uint32_t computed_crc = Crc32(
        reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
    if (computed_crc != stored_crc) {
      break;
    }

    WalRecord rec;
    rec.type = static_cast<WalOpType>(type_byte);
    rec.doc_id = std::move(doc_id);
    rec.json = std::move(json);
    records.push_back(std::move(rec));
  }

  close(fd);
  return records;
}

void WriteAheadLog::Truncate() {
  if (ftruncate(fd_, 0) != 0) {
    throw std::system_error(errno, std::generic_category(),
                             "WriteAheadLog: ftruncate failed for " + path_);
  }
  if (lseek(fd_, 0, SEEK_SET) < 0) {
    throw std::system_error(errno, std::generic_category(),
                             "WriteAheadLog: lseek failed for " + path_);
  }
}

uint64_t WriteAheadLog::SizeBytes() const {
  off_t cur = lseek(fd_, 0, SEEK_CUR);
  off_t end = lseek(fd_, 0, SEEK_END);
  if (cur >= 0) {
    lseek(fd_, cur, SEEK_SET);
  }
  if (end < 0) {
    throw std::system_error(errno, std::generic_category(),
                             "WriteAheadLog: lseek failed for " + path_);
  }
  return static_cast<uint64_t>(end);
}

}  // namespace dse::wal
