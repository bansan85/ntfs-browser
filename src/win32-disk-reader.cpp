#include <ntfs-browser/win32-disk-reader.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{

Win32DiskReader::Win32DiskReader()
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle))
{
}

bool Win32DiskReader::Open(std::wstring_view path)
{
  handle_ =
      HandlePtr(CreateFileW(path.data(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr),
                &CloseHandle);
  return handle_.get() != INVALID_HANDLE_VALUE;
}

bool Win32DiskReader::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const
{
  DWORD len = SetFilePointer(handle_.get(), static_cast<LONG>(addr.LowPart),
                             &addr.HighPart, FILE_BEGIN);

  if (len == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
  {
    NTFS_TRACE1("Cannot set file pointer to %I64d\n", addr.QuadPart);
    return false;
  }

  if (ReadFile(handle_.get(), dest.data(), static_cast<DWORD>(dest.size()),
               &len, nullptr) == FALSE ||
      len != dest.size())
  {
    NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
    return false;
  }

  return true;
}

}  // namespace NtfsBrowser
