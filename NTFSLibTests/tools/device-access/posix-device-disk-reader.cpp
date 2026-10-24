#ifndef _WIN32

  #include "posix-device-disk-reader.h"

  #include <cerrno>
  #include <cstdint>
  #include <format>
  #include <iostream>
  #include <limits>
  #include <optional>
  #include <span>
  #include <string>
  #include <string_view>

  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  #ifdef __linux__
    #include <linux/fs.h>
  #endif

  #include "ntfs-browser/win-types.h"
  #include "text-convert.h"

namespace NtfsDevice {

PosixDeviceDiskReader::~PosixDeviceDiskReader() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool PosixDeviceDiskReader::Open(std::wstring_view path) {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }

  const std::string narrow = WideToUtf8(path);
  // POSIX open() is variadic and has no fixed-arity form.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  fd_ = open(narrow.c_str(), O_RDONLY);
  last_error_ = fd_ >= 0 ? 0 : errno;
  return fd_ >= 0;
}

bool PosixDeviceDiskReader::ReadInto(LARGE_INTEGER& addr,
                                     std::span<BYTE> dest) const {
  const ssize_t bytes_read =
      pread(fd_, dest.data(), dest.size(), addr.QuadPart);
  if (bytes_read < 0 || static_cast<size_t>(bytes_read) != dest.size()) {
    std::cerr << std::format("Cannot read device at offset {}\n",
                             addr.QuadPart);
    return false;
  }
  return true;
}

std::optional<ULONGLONG> PosixDeviceDiskReader::Size() const {
  struct stat info{};
  if (fd_ < 0 || fstat(fd_, &info) != 0) {
    return std::nullopt;
  }
  if (S_ISREG(info.st_mode)) {
    return static_cast<ULONGLONG>(info.st_size);
  }
  #ifdef BLKGETSIZE64
  if (S_ISBLK(info.st_mode)) {
    std::uint64_t bytes = 0;
    // ioctl() is variadic and has no fixed-arity form.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (ioctl(fd_, BLKGETSIZE64, &bytes) == 0) {
      return bytes;
    }
  }
  #endif
  return std::nullopt;
}

void PosixDeviceDiskReader::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  #ifdef POSIX_FADV_WILLNEED
  constexpr auto max_offset =
      static_cast<ULONGLONG>(std::numeric_limits<off_t>::max());
  if (fd_ < 0 || offset > max_offset || length > max_offset - offset) {
    return;
  }
  // The kernel starts the reads into the page cache and returns at once. A
  // failed hint costs nothing: pread() reads the bytes anyway.
  static_cast<void>(posix_fadvise(fd_, static_cast<off_t>(offset),
                                  static_cast<off_t>(length),
                                  POSIX_FADV_WILLNEED));
  #else
  static_cast<void>(offset);
  static_cast<void>(length);
  #endif
}

}  // namespace NtfsDevice

#endif  // !_WIN32
