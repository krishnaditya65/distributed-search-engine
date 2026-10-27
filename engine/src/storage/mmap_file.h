#ifndef DSE_STORAGE_MMAP_FILE_H_
#define DSE_STORAGE_MMAP_FILE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace dse::storage {

// RAII wrapper around a POSIX mmap()'d file.
//
// MMapFile provides zero-copy access to file contents by mapping the file
// directly into the process's address space. This is the backing mechanism
// used to read the search engine's on-disk immutable segments (the OS page
// cache serves reads directly out of the mapped region, with no extra copy
// into application-managed heap memory) and to let a segment writer build a
// file's final on-disk layout directly via Data().
//
// Instances are move-only: copying would require either duplicating the
// mapping (semantically confusing) or reference counting (unnecessary
// complexity for this module), so it is simply disallowed.
//
// Mapping choice for OpenReadOnly(): MAP_SHARED is used (rather than
// MAP_PRIVATE) even though the mapping is PROT_READ-only. For a read-only
// mapping the two are observationally identical for this class's callers
// (no writes are ever issued through a read-only mapping, so there is
// nothing to keep private/copy-on-write); MAP_SHARED is chosen simply to
// keep the flag consistent with CreateAndMap() and to allow the OS to share
// the same physical pages across multiple read-only mappers of the same
// file.
//
// Note: mmap() of a zero-length region is invalid (POSIX says the behavior
// is undefined and glibc/Darwin both fail the call), so:
//   - OpenReadOnly() throws std::runtime_error if the file is empty.
//   - CreateAndMap() requires size > 0 and throws std::runtime_error
//     otherwise; callers must not attempt to create a zero-sized mapped
//     file with this class.
class MMapFile {
 public:
  // Creates (or truncates/overwrites) a file at `path`, sizes it to exactly
  // `size` bytes (via ftruncate), and maps it PROT_READ|PROT_WRITE,
  // MAP_SHARED so writes to Data() are eventually persisted to disk (and
  // immediately visible to Sync()). `size` must be > 0; throws
  // std::runtime_error otherwise, and on any OS-level failure (open,
  // ftruncate, mmap).
  static MMapFile CreateAndMap(const std::string& path, size_t size);

  // Opens an existing file read-only and maps it PROT_READ, MAP_SHARED (see
  // class comment for why MAP_SHARED is used for a read-only mapping) for
  // zero-copy reads. Maps the file's full current size, determined via
  // fstat. Throws std::runtime_error if the file cannot be opened/stat'd,
  // or if the file is empty (mapping zero bytes is invalid).
  static MMapFile OpenReadOnly(const std::string& path);

  MMapFile(MMapFile&& other) noexcept;
  MMapFile& operator=(MMapFile&& other) noexcept;

  MMapFile(const MMapFile&) = delete;
  MMapFile& operator=(const MMapFile&) = delete;

  // Must munmap + close exactly once, and be safe to call on a moved-from
  // (empty) instance without double-unmapping. Never throws.
  ~MMapFile();

  uint8_t* Data();
  const uint8_t* Data() const;
  size_t Size() const;
  bool IsWritable() const;

  // Flushes dirty pages to disk (msync(MS_SYNC)). No-op / safe to call on a
  // read-only mapping or an empty (moved-from) instance. Never throws;
  // errors are swallowed.
  void Sync();

 private:
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  bool writable_ = false;

  MMapFile() = default;  // Used internally by the two factory methods.

  void ResetToEmpty();
};

}  // namespace dse::storage

#endif  // DSE_STORAGE_MMAP_FILE_H_
