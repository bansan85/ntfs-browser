#pragma once

#include <ntfs-browser/win-types.h>

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Forwards every read to another IDiskReader and appends the bytes it served
// to a buffer, in read order and without their address. It does what
// Test::DiskReaders::RecordingDiskReader does, but in memory.
class RecordingReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // Records what inner serves into sink. sink MUST outlive the reader.
  RecordingReader(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner,
                  std::vector<BYTE>& sink);

  bool Open(std::wstring_view path) override;

  // A failed read cannot be replayed from a stream: it throws
  // std::runtime_error instead of being recorded.
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The size of the wrapped reader. It reads nothing.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

 private:
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner_;
  std::vector<BYTE>* sink_;
  mutable std::mutex mutex_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
