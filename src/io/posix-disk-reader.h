#pragma once

#ifndef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <optional>
  #include <span>
  #include <string_view>

  #include <ntfs-browser/disk-reader.h>
  #include <ntfs-browser/namespace.h>

  #include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// IDiskReader of a POSIX host: an image file or a block device, opened
// read-only and read with pread(). The partition node of a device already
// counts its offsets from the start of the partition. It gives no media kind
// and no read ahead: the page cache does that.
class NTFS_BROWSER_EXPORT_TESTS_ONLY PosixDiskReader : public IDiskReader {
 public:
  PosixDiskReader() = default;
  PosixDiskReader(const PosixDiskReader&) = delete;
  PosixDiskReader& operator=(const PosixDiskReader&) = delete;
  PosixDiskReader(PosixDiskReader&&) = delete;
  PosixDiskReader& operator=(PosixDiskReader&&) = delete;
  ~PosixDiskReader() override;

  // Opens the file or device at path, a UTF-16 path on a platform where
  // wchar_t is wide: it is turned into UTF-8 for the system. Returns false
  // when it cannot, and Error() then gives the errno.
  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The size of an image file or of a block device. Nothing for any other kind
  // of file.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

  // The errno of the last Open() that failed, 0 after one that succeeded.
  [[nodiscard]] int Error() const noexcept { return error_; }

 private:
  int fd_{-1};
  int error_{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io

#endif  // !_WIN32
