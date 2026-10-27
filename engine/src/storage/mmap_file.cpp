#include "mmap_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace dse::storage {

namespace {

[[noreturn]] void ThrowErrno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace

MMapFile MMapFile::CreateAndMap(const std::string& path, size_t size) {
  if (size == 0) {
    throw std::runtime_error(
        "MMapFile::CreateAndMap: size must be > 0 (path=" + path + ")");
  }

  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    ThrowErrno("MMapFile::CreateAndMap: open(" + path + ") failed");
  }

  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    int saved_errno = errno;
    ::close(fd);
    errno = saved_errno;
    ThrowErrno("MMapFile::CreateAndMap: ftruncate(" + path + ") failed");
  }

  void* mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, 0);
  if (mapped == MAP_FAILED) {
    int saved_errno = errno;
    ::close(fd);
    errno = saved_errno;
    ThrowErrno("MMapFile::CreateAndMap: mmap(" + path + ") failed");
  }

  MMapFile file;
  file.fd_ = fd;
  file.data_ = static_cast<uint8_t*>(mapped);
  file.size_ = size;
  file.writable_ = true;
  return file;
}

MMapFile MMapFile::OpenReadOnly(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    ThrowErrno("MMapFile::OpenReadOnly: open(" + path + ") failed");
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    int saved_errno = errno;
    ::close(fd);
    errno = saved_errno;
    ThrowErrno("MMapFile::OpenReadOnly: fstat(" + path + ") failed");
  }

  if (st.st_size <= 0) {
    ::close(fd);
    throw std::runtime_error(
        "MMapFile::OpenReadOnly: cannot mmap empty file (path=" + path +
        ")");
  }
  size_t size = static_cast<size_t>(st.st_size);

  void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    int saved_errno = errno;
    ::close(fd);
    errno = saved_errno;
    ThrowErrno("MMapFile::OpenReadOnly: mmap(" + path + ") failed");
  }

  MMapFile file;
  file.fd_ = fd;
  file.data_ = static_cast<uint8_t*>(mapped);
  file.size_ = size;
  file.writable_ = false;
  return file;
}

MMapFile::MMapFile(MMapFile&& other) noexcept
    : fd_(other.fd_),
      data_(other.data_),
      size_(other.size_),
      writable_(other.writable_) {
  other.ResetToEmpty();
}

MMapFile& MMapFile::operator=(MMapFile&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (data_ != nullptr) {
    ::munmap(data_, size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }

  fd_ = other.fd_;
  data_ = other.data_;
  size_ = other.size_;
  writable_ = other.writable_;
  other.ResetToEmpty();
  return *this;
}

void MMapFile::ResetToEmpty() {
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
  writable_ = false;
}

MMapFile::~MMapFile() {
  if (data_ != nullptr) {
    if (::munmap(data_, size_) != 0) {
      std::fprintf(stderr, "MMapFile: munmap failed: %s\n",
                   std::strerror(errno));
    }
  }
  if (fd_ >= 0) {
    if (::close(fd_) != 0) {
      std::fprintf(stderr, "MMapFile: close failed: %s\n",
                   std::strerror(errno));
    }
  }
}

uint8_t* MMapFile::Data() { return data_; }

const uint8_t* MMapFile::Data() const { return data_; }

size_t MMapFile::Size() const { return size_; }

bool MMapFile::IsWritable() const { return writable_; }

void MMapFile::Sync() {
  if (data_ == nullptr) {
    return;
  }
  if (::msync(data_, size_, MS_SYNC) != 0) {
    std::fprintf(stderr, "MMapFile: msync failed: %s\n",
                 std::strerror(errno));
  }
}

}  // namespace dse::storage
