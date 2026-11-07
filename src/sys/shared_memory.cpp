#include "softswitch/sys/shared_memory.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

namespace softswitch::sys {
namespace {

void* map(int fd, std::size_t size) {
  void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
  if (addr == MAP_FAILED) {  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast): libc macro
    throw_errno("mmap");
  }
  return addr;
}

}  // namespace

SharedMemory::~SharedMemory() { unmap(); }

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : fd_(std::move(other.fd_)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
  if (this != &other) {
    unmap();
    fd_ = std::move(other.fd_);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

void SharedMemory::unmap() noexcept {
  if (data_ != nullptr) {
    ::munmap(data_, size_);
    data_ = nullptr;
  }
}

SharedMemory SharedMemory::create(const std::string& name, std::size_t size) {
  UniqueFd memfd{::memfd_create(name.c_str(), MFD_CLOEXEC | MFD_ALLOW_SEALING)};
  if (!memfd.valid()) {
    throw_errno("memfd_create");
  }
  if (::ftruncate(memfd.get(), static_cast<off_t>(size)) < 0) {
    throw_errno("ftruncate");
  }
  // The peer can't resize the region underneath us.
  if (::fcntl(memfd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
    throw_errno("fcntl(F_ADD_SEALS)");
  }
  void* mapping = map(memfd.get(), size);
  return SharedMemory{std::move(memfd), mapping, size};
}

SharedMemory SharedMemory::attach(UniqueFd fd) {
  struct stat st {};
  if (::fstat(fd.get(), &st) < 0) {
    throw_errno("fstat");
  }
  const auto bytes = static_cast<std::size_t>(st.st_size);
  void* mapping = map(fd.get(), bytes);
  return SharedMemory{std::move(fd), mapping, bytes};
}

}  // namespace softswitch::sys
