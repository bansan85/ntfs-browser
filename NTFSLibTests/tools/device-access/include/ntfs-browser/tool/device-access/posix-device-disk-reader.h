#pragma once

#ifndef _WIN32

  #include <optional>
  #include <span>
  #include <string_view>

  #include <ntfs-browser/namespace.h>

  #include "disk-reader.h"
  #include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// Production IDiskReader for Linux: opens a raw block device node (eg.
// "/dev/sdb1") or an image file read-only and reads it with pread(). A
// partition's own device node already exposes offsets relative to the
// partition's start, so unlike a whole-disk image file, no separate
// offsetting is needed.
class PosixDeviceDiskReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  PosixDeviceDiskReader() = default;
  PosixDeviceDiskReader(const PosixDeviceDiskReader&) = delete;
  PosixDeviceDiskReader& operator=(const PosixDeviceDiskReader&) = delete;
  PosixDeviceDiskReader(PosixDeviceDiskReader&&) = delete;
  PosixDeviceDiskReader& operator=(PosixDeviceDiskReader&&) = delete;
  ~PosixDeviceDiskReader() override;

  bool Open(std::wstring_view path) override;
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The size of an image file, or of a block device. Nullopt for any other
  // kind of file.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

  // The kind of block device, from sysfs ("rotational"). For an image file, the
  // kind of the block device that holds its file system. Unknown when sysfs
  // does not say, as for a file under a network or pseudo file system.
  [[nodiscard]] NTFS_BROWSER_NAMESPACE::Tuning::MediaKind
      GetMediaKind() const override;

  // Asks the kernel to read the range into the page cache ahead of pread(),
  // where posix_fadvise() exists. The page cache holds what it read, so there
  // is nothing to cancel.
  void Prefetch(ULONGLONG offset, ULONGLONG length) const override;

  // Does what Prefetch() does. The kernel keeps no lane of ranges, so
  // CancelStanding() has nothing to withdraw and stays the default.
  void PrefetchStanding(ULONGLONG offset, ULONGLONG length) const override;

  // The errno of the last Open() that failed, 0 after one that succeeded.
  // Callers tell "permission denied" from "no such file" with it, since
  // Open() itself only returns a bool.
  [[nodiscard]] int LastError() const noexcept { return last_error_; }

 private:
  int fd_ = -1;
  NTFS_BROWSER_NAMESPACE::Tuning::MediaKind media_kind_ =
      NTFS_BROWSER_NAMESPACE::Tuning::MediaKind::Unknown;
  int last_error_ = 0;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess

#endif  // !_WIN32
