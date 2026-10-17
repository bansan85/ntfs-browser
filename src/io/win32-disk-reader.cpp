#include "io/win32-disk-reader.h"

#include <string>

#include <gsl/narrow>

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
  DWORD len = SetFilePointer(handle_.get(), static_cast<LONG>(addr.LowPart),
                             &addr.HighPart, FILE_BEGIN);

  if (len == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
    Log::Error("Cannot set file pointer to {}", addr.QuadPart);
    return false;
  }

  if (ReadFile(handle_.get(), dest.data(), gsl::narrow<DWORD>(dest.size()),
               &len, nullptr) == FALSE ||
      len != dest.size()) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return false;
  }

  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
