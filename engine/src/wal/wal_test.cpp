#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using dse::wal::WalOpType;
using dse::wal::WalRecord;
using dse::wal::WriteAheadLog;

namespace {

std::string MakeTempPath() {
  static int counter = 0;
  ++counter;
  return "/tmp/dse_wal_test_" + std::to_string(getpid()) + "_" +
         std::to_string(counter) + ".log";
}

bool RecordsEqual(const WalRecord& a, const WalRecord& b) {
  return a.type == b.type && a.doc_id == b.doc_id && a.json == b.json;
}

void AssertRecordsEqual(const std::vector<WalRecord>& got,
                         const std::vector<WalRecord>& want) {
  assert(got.size() == want.size());
  for (size_t i = 0; i < got.size(); ++i) {
    assert(RecordsEqual(got[i], want[i]));
  }
}

void AppendLE32Raw(std::string* buf, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    buf->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

}  // namespace

int main() {
  const std::string path = MakeTempPath();
  ::unlink(path.c_str());  // start clean

  std::vector<WalRecord> expected;
  expected.push_back(WalRecord{WalOpType::kIndex, "doc1", "{\"a\":1}"});
  expected.push_back(WalRecord{WalOpType::kIndex, "doc2", "{\"b\":2}"});
  expected.push_back(WalRecord{WalOpType::kDelete, "doc1", ""});
  expected.push_back(WalRecord{WalOpType::kIndex, "doc3", "{\"c\":3,\"d\":\"x\"}"});
  expected.push_back(WalRecord{WalOpType::kDelete, "doc3", ""});

  // 1. Append a handful of records and read them back.
  {
    WriteAheadLog wal(path);
    for (const auto& rec : expected) {
      wal.Append(rec);
    }
    std::vector<WalRecord> got = wal.ReadAll();
    AssertRecordsEqual(got, expected);
  }

  // 2. Close/reopen (simulate restart); durability check.
  {
    WriteAheadLog wal(path);
    std::vector<WalRecord> got = wal.ReadAll();
    AssertRecordsEqual(got, expected);
  }

  // 3. Simulate a torn tail write.
  {
    // Record the size of the valid file before corrupting it.
    uint64_t good_size;
    {
      WriteAheadLog wal(path);
      good_size = wal.SizeBytes();
    }

    // Manually append a well-formed-looking-but-truncated record: a valid
    // length prefix claiming a doc_id of 10 bytes, but only supply 3.
    int fd = open(path.c_str(), O_WRONLY | O_APPEND);
    assert(fd >= 0);
    std::string garbage;
    AppendLE32Raw(&garbage, 10);  // claims doc_id_len = 10
    garbage.append("abc");        // but only 3 bytes follow, then EOF
    ssize_t n = write(fd, garbage.data(), garbage.size());
    assert(n == static_cast<ssize_t>(garbage.size()));
    close(fd);

    WriteAheadLog wal(path);
    std::vector<WalRecord> got = wal.ReadAll();
    AssertRecordsEqual(got, expected);
    // The file itself grew (torn tail present), but ReadAll should still
    // only surface the valid prefix.
    assert(wal.SizeBytes() > good_size);
  }

  // 3b. Also test a full record with a corrupted CRC byte.
  {
    std::string path2 = MakeTempPath();
    ::unlink(path2.c_str());
    std::vector<WalRecord> expected2;
    expected2.push_back(WalRecord{WalOpType::kIndex, "docA", "{\"z\":9}"});

    {
      WriteAheadLog wal(path2);
      wal.Append(expected2[0]);
    }

    // Manually append a full, well-formed record, then flip a byte in its
    // CRC field so it fails validation.
    {
      int fd = open(path2.c_str(), O_WRONLY | O_APPEND);
      assert(fd >= 0);
      std::string frame;
      std::string doc_id = "docB";
      std::string json = "{\"bad\":true}";
      AppendLE32Raw(&frame, static_cast<uint32_t>(doc_id.size()));
      frame.append(doc_id);
      frame.push_back(static_cast<char>(0));  // kIndex
      AppendLE32Raw(&frame, static_cast<uint32_t>(json.size()));
      frame.append(json);
      // Deliberately wrong CRC (all zero bytes, almost certainly incorrect).
      AppendLE32Raw(&frame, 0);
      ssize_t n = write(fd, frame.data(), frame.size());
      assert(n == static_cast<ssize_t>(frame.size()));
      close(fd);
    }

    WriteAheadLog wal(path2);
    std::vector<WalRecord> got = wal.ReadAll();
    AssertRecordsEqual(got, expected2);

    ::unlink(path2.c_str());
  }

  // 4. Test Truncate().
  {
    WriteAheadLog wal(path);
    wal.Truncate();
    assert(wal.ReadAll().empty());
    assert(wal.SizeBytes() == 0);

    WalRecord rec{WalOpType::kIndex, "docX", "{\"e\":5}"};
    wal.Append(rec);
    std::vector<WalRecord> got = wal.ReadAll();
    assert(got.size() == 1);
    assert(RecordsEqual(got[0], rec));
  }

  ::unlink(path.c_str());

  std::printf("wal tests passed\n");
  return 0;
}
