#ifndef _WIN32

  #include "posix-device-disk-reader.h"

  #include <cerrno>
  #include <cstdint>
  #include <cstdlib>
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
    #include <sys/sysmacros.h>
  #endif

  #include "disk-identity.h"
  #include "host-media.h"
  #include "ntfs-browser/win-types.h"
  #include "text-convert.h"

namespace NtfsDevice {

namespace {

using NTFS_BROWSER_NAMESPACE::Tuning::MediaKind;

MediaKind KindOf(bool solid_state) {
  return solid_state ? MediaKind::SolidState : MediaKind::Rotational;
}

// A block device says its own kind. An image file says the kind of the disk
// that holds the file system it sits on, where sysfs knows that disk.
MediaKind MediaKindOf(int fd, std::wstring_view path) {
  struct stat info{};
  if (fstat(fd, &info) != 0) {
    return MediaKind::Unknown;
  }
  if (S_ISREG(info.st_mode)) {
  #ifdef __linux__
    // Set by the unit tests, so that the tools they run as child processes ask
    // no sysfs either.
    if (std::getenv("NTFS_BROWSER_NO_HOST_MEDIA_QUERY") != nullptr) {
      return MediaKind::Unknown;
    }
    const std::optional<bool> rotational = RotationalOfBlockDevice(
        major(info.st_dev), minor(info.st_dev), SysfsRoot());
    return rotational ? KindOf(!*rotational) : MediaKind::Unknown;
  #else
    return MediaKind::Unknown;
  #endif
  }
  const std::optional<DiskIdentity> disk = IdentifyDisk(path);
  return disk && disk->solid_state ? KindOf(*disk->solid_state)
                                   : MediaKind::Unknown;
}

}  // namespace

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
  media_kind_ = NTFS_BROWSER_NAMESPACE::Tuning::MediaKind::Unknown;
  if (fd_ >= 0) {
    media_kind_ = MediaKindOf(fd_, path);
  }
  return fd_ >= 0;
}

NTFS_BROWSER_NAMESPACE::Tuning::MediaKind
    PosixDeviceDiskReader::GetMediaKind() const {
  return media_kind_;
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

void PosixDeviceDiskReader::PrefetchStanding(ULONGLONG offset,
                                             ULONGLONG length) const {
  Prefetch(offset, length);
}

}  // namespace NtfsDevice

#endif  // !_WIN32
