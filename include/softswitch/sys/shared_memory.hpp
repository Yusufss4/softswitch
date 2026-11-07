#pragma once

/// @file
/// memfd-backed shared memory that can be handed to another process as a descriptor.

#include <cstddef>
#include <string>

#include "softswitch/sys/fd.hpp"

namespace softswitch::sys {

/// A MAP_SHARED mapping of a memfd. Move-only; unmaps on destruction.
class SharedMemory {
 public:
  SharedMemory() = default;
  ~SharedMemory();
  SharedMemory(SharedMemory&& other) noexcept;
  SharedMemory& operator=(SharedMemory&& other) noexcept;
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

  /// Creates a sealed-size memfd of @p size bytes, maps it and prefaults every page
  /// (MAP_POPULATE) so the real-time reader never takes a page fault on it.
  [[nodiscard]] static SharedMemory create(const std::string& name, std::size_t size);

  /// Maps an existing memfd received from another process.
  [[nodiscard]] static SharedMemory attach(UniqueFd fd);

  [[nodiscard]] int fd() const noexcept { return fd_.get(); }
  [[nodiscard]] void* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  SharedMemory(UniqueFd fd, void* data, std::size_t size) noexcept
      : fd_(std::move(fd)), data_(data), size_(size) {}
  void unmap() noexcept;

  UniqueFd fd_;
  void* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace softswitch::sys
