#pragma once

#ifndef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <optional>
  #include <span>
  #include <string_view>

  #include <ntfs-browser/disk-reader.h>
  #include <ntfs-browser/namespace.h>

namespace NtfsDevice {

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

  // The errno of the last Open() that failed, 0 after one that succeeded.
  // Callers tell "permission denied" from "no such file" with it, since
  // Open() itself only returns a bool.
  [[nodiscard]] int LastError() const noexcept { return last_error_; }

 private:
  int fd_ = -1;
  int last_error_ = 0;
};

}  // namespace NtfsDevice

#endif  // !_WIN32
