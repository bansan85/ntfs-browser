#ifndef _WIN32

  #include "posix-device-disk-reader.h"

  #include <cerrno>
  #include <format>
  #include <iostream>
  #include <span>
  #include <string>
  #include <string_view>

  #include <fcntl.h>
  #include <sys/types.h>
  #include <unistd.h>

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

}  // namespace NtfsDevice

#endif  // !_WIN32
