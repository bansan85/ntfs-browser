#pragma once

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Forwards every read to another IDiskReader and appends the bytes it served
// to a trace file, in read order and without their address. A
// SequentialDiskReader over MakeFileStreamProducer() replays that file, so a
// test needs none of the bytes it never read from a large image. A failed read
// cannot be replayed: it throws instead of being recorded.
class RecordingDiskReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // Creates trace_path, and its parent directory if need be.
  RecordingDiskReader(
      std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner,
      const std::filesystem::path& trace_path);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The size of the wrapped reader. It reads nothing, so the trace is the
  // same.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

 private:
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner_;
  mutable std::ofstream trace_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
