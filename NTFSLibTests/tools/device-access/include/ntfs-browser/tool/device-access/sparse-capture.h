#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <ntfs-browser/namespace.h>

#include "bench/bench.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// Writes what a run reads into a sparse file, at the offset of each read. The
// file is an image of the ranges the run used and of nothing else: the tool
// run again on it reads the same bytes from the page cache, with no disk
// involved. A range of zeros is not written, so it stays a hole.
class SparseCapture final : public NTFS_BROWSER_NAMESPACE::Bench::ICaptureSink {
 public:
  // Creates path, empty and sparse. Check Ok() before use.
  explicit SparseCapture(const std::filesystem::path& path);
  ~SparseCapture() override;
  SparseCapture(const SparseCapture&) = delete;
  SparseCapture& operator=(const SparseCapture&) = delete;
  SparseCapture(SparseCapture&&) = delete;
  SparseCapture& operator=(SparseCapture&&) = delete;

  [[nodiscard]] bool Ok() const noexcept;

  // The reason a write or the creation failed, or empty.
  [[nodiscard]] const std::string& Error() const noexcept { return error_; }

  void Write(std::uint64_t offset, std::span<const BYTE> bytes) override;

  // Gives the file the size of the volume, so that the run on the image sees a
  // store as large as the real one. Bytes written beyond it stay.
  bool SetSize(std::uint64_t bytes);

 private:
  // Platform handle, hidden behind the pointer.
  class Impl;

  std::unique_ptr<Impl> impl_;
  std::string error_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
