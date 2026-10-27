#include "mmap_file.h"

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>

using dse::storage::MMapFile;

namespace {

std::string TempPath(const char* suffix) {
  return "/tmp/dse_mmap_file_test_" + std::to_string(::getpid()) + "_" +
         suffix;
}

}  // namespace

int main() {
  const std::string path = TempPath("main");
  const size_t kSize = 4096;

  // 1. Create, write a known byte pattern, sync.
  {
    MMapFile file = MMapFile::CreateAndMap(path, kSize);
    assert(file.Size() == kSize);
    assert(file.IsWritable());
    uint8_t* data = file.Data();
    for (size_t i = 0; i < kSize; ++i) {
      data[i] = static_cast<uint8_t>(i % 256);
    }
    file.Sync();
  }  // 2. Destroyed here (out of scope) -> munmap + close.

  // 2. Reopen read-only and verify the pattern reached disk.
  {
    MMapFile file = MMapFile::OpenReadOnly(path);
    assert(file.Size() == kSize);
    assert(!file.IsWritable());
    const uint8_t* data = file.Data();
    for (size_t i = 0; i < kSize; ++i) {
      assert(data[i] == static_cast<uint8_t>(i % 256));
    }

    // 3. Move construction.
    MMapFile moved(std::move(file));
    assert(moved.Size() == kSize);
    assert(!moved.IsWritable());
    assert(moved.Data() != nullptr);
    for (size_t i = 0; i < kSize; ++i) {
      assert(moved.Data()[i] == static_cast<uint8_t>(i % 256));
    }
    // `file` is now moved-from (empty); its destructor at scope exit must
    // be a safe no-op (no double munmap/close of `moved`'s resources).

    // Move assignment into a freshly-created (different) mapping, which
    // should be cleanly torn down before absorbing `moved`'s state.
    const std::string other_path = TempPath("other");
    MMapFile target = MMapFile::CreateAndMap(other_path, 128);
    assert(target.Size() == 128);
    target = std::move(moved);
    assert(target.Size() == kSize);
    assert(!target.IsWritable());
    for (size_t i = 0; i < kSize; ++i) {
      assert(target.Data()[i] == static_cast<uint8_t>(i % 256));
    }
    // `moved` is now moved-from as well; both `file` and `moved` destruct
    // safely as no-ops when this block ends. `target` owns the mapping.
    ::unlink(other_path.c_str());
  }

  // 4. IsWritable() reflects CreateAndMap == true, OpenReadOnly == false.
  {
    const std::string writable_path = TempPath("writable");
    MMapFile writable_file = MMapFile::CreateAndMap(writable_path, 64);
    assert(writable_file.IsWritable());
    writable_file.Sync();
    MMapFile ro_file = MMapFile::OpenReadOnly(writable_path);
    assert(!ro_file.IsWritable());
    ::unlink(writable_path.c_str());
  }

  // 5. CreateAndMap with size=0 must throw std::runtime_error (documented,
  // consistent behavior: zero-length mmap is invalid, so we reject it at
  // the API boundary rather than attempting an invalid mmap call).
  {
    bool threw = false;
    try {
      MMapFile zero = MMapFile::CreateAndMap(TempPath("zero"), 0);
      (void)zero;
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
  }

  ::unlink(path.c_str());

  std::printf("mmap file tests passed\n");
  return 0;
}
