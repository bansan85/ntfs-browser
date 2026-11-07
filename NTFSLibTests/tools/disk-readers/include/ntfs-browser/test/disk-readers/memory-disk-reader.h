#pragma once

#include <ntfs-browser/win-types.h>

#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// A fake IDiskReader backed entirely by an in-memory buffer.
class MemoryDiskReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // data is the whole fake volume/file content, ready to be read from.
  explicit MemoryDiskReader(std::vector<BYTE> data);

  // Loads a real file's content once, so subsequent reads never touch disk.
  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The length of the buffer.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

  // The kind of device that the reader says it is, Unknown until
  // SetMediaKind(): a test of a device-dependent choice plays the device.
  [[nodiscard]] Tuning::MediaKind GetMediaKind() const override;
  void SetMediaKind(Tuning::MediaKind kind) noexcept;

 private:
  std::vector<BYTE> data_;
  Tuning::MediaKind kind_{Tuning::MediaKind::Unknown};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
