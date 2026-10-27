#ifndef DSE_WAL_WAL_H_
#define DSE_WAL_WAL_H_

#include <cstdint>
#include <string>
#include <vector>

namespace dse::wal {

enum class WalOpType : uint8_t { kIndex = 0, kDelete = 1 };

struct WalRecord {
  WalOpType type;
  std::string doc_id;
  std::string json;  // populated only for kIndex; empty for kDelete
};

class WriteAheadLog {
 public:
  // Opens the file at `path` for append (creating it if missing, preserving
  // existing content if present).
  explicit WriteAheadLog(const std::string& path);
  ~WriteAheadLog();

  WriteAheadLog(const WriteAheadLog&) = delete;
  WriteAheadLog& operator=(const WriteAheadLog&) = delete;

  // Serializes `record`, appends it to the file, and fsyncs before returning
  // (durability: once this call returns, the record survives a crash).
  void Append(const WalRecord& record);

  // Reads the ENTIRE file from the beginning and returns all successfully
  // parsed records in order. If a torn/partial write is detected at the tail
  // (e.g. process crashed mid-Append), stop reading at that point and return
  // everything read successfully up to there — do not throw, do not include
  // the corrupt tail record. This method does not affect the current append
  // position; it always reads from byte 0 via a fresh file handle/fd.
  std::vector<WalRecord> ReadAll() const;

  // Truncates the log to empty (called after the shard successfully flushes
  // the corresponding in-memory state to an immutable on-disk segment, since
  // the WAL's job for that data is now done).
  void Truncate();

  uint64_t SizeBytes() const;

 private:
  std::string path_;
  int fd_ = -1;  // append-mode fd, or your chosen mechanism
};

}  // namespace dse::wal

#endif  // DSE_WAL_WAL_H_
