#include "io/win32-disk-reader.h"

#include <optional>
#include <string>

#include <gsl/narrow>
#include <winioctl.h>

#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

Win32DiskReader::Win32DiskReader()
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle)) {}

bool Win32DiskReader::Open(std::wstring_view path) {
  // A view carries no NUL terminator, and CreateFileW needs one.
  const std::wstring path_z(path);
  // SEQUENTIAL_SCAN is only a hint: it asks the cache manager for more
  // read-ahead and earlier eviction of pages already read, so an $MFT scan
  // does not flood the system cache. A random read stays correct.
  handle_ = HandlePtr(
      CreateFileW(path_z.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_READONLY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr),
      &CloseHandle);
  return handle_.get() != INVALID_HANDLE_VALUE;
}

bool Win32DiskReader::ReadInto(LARGE_INTEGER& addr,
                               std::span<BYTE> dest) const {
  // OVERLAPPED carries the offset as unsigned: a negative one would wrap.
  if (addr.QuadPart < 0) {
    Log::Error("Cannot read file at adress {}: negative address",
               addr.QuadPart);
    return false;
  }

  // The handle is synchronous, so OVERLAPPED only carries the offset. ReadFile
  // reads there and ignores the handle's own file pointer.
  OVERLAPPED overlapped{};
  overlapped.Offset = addr.LowPart;
  overlapped.OffsetHigh = gsl::narrow<DWORD>(addr.HighPart);

  DWORD len = 0;
  if (ReadFile(handle_.get(), dest.data(), gsl::narrow<DWORD>(dest.size()),
               &len, &overlapped) == FALSE ||
      len != dest.size()) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return false;
  }

  return true;
}

std::optional<ULONGLONG> Win32DiskReader::Size() const {
  // Answers for a disk or a volume handle, and fails for an ordinary file.
  GET_LENGTH_INFORMATION length_info{};
  DWORD returned = 0;
  if (DeviceIoControl(handle_.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                      &length_info, sizeof(length_info), &returned,
                      nullptr) != FALSE &&
      length_info.Length.QuadPart >= 0) {
    return gsl::narrow<ULONGLONG>(length_info.Length.QuadPart);
  }

  LARGE_INTEGER file_size{};
  if (GetFileSizeEx(handle_.get(), &file_size) != FALSE &&
      file_size.QuadPart >= 0) {
    return gsl::narrow<ULONGLONG>(file_size.QuadPart);
  }
  return std::nullopt;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
