#ifndef _WIN32

  #include "io/posix-disk-reader.h"

  #include <cerrno>
  #include <cstddef>
  #include <cstdint>
  #include <limits>
  #include <optional>
  #include <span>
  #include <string>
  #include <string_view>

  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  #ifdef __linux__
    #include <linux/fs.h>
    #include <sys/ioctl.h>
  #endif

  #include <ntfs-browser/namespace.h>

  #include "log/ntfs-common.h"
  #include "utf/utf.h"
  #include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

PosixDiskReader::~PosixDiskReader() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool PosixDiskReader::Open(std::wstring_view path) {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  const std::string narrow = Utf::WideToUtf8(path);
  // POSIX open() is variadic and has no fixed-arity form.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  fd_ = open(narrow.c_str(), O_RDONLY | O_CLOEXEC);
  error_ = fd_ >= 0 ? 0 : errno;
  return fd_ >= 0;
}

bool PosixDiskReader::ReadInto(LARGE_INTEGER& addr,
                               std::span<BYTE> dest) const {
  if (fd_ < 0 || addr.QuadPart < 0) {
    return false;
  }
  // pread() may return less than asked on a device: go on until dest is full.
  std::size_t done = 0;
  while (done < dest.size()) {
    const ssize_t got =
        pread(fd_, dest.data() + done, dest.size() - done,
              static_cast<off_t>(addr.QuadPart) + static_cast<off_t>(done));
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      Log::Debug("Cannot read device at offset {}", addr.QuadPart);
      return false;
    }
    done += static_cast<std::size_t>(got);
  }
  return true;
}

std::optional<ULONGLONG> PosixDiskReader::Size() const {
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

}  // namespace NTFS_BROWSER_NAMESPACE::Io

#endif  // !_WIN32
