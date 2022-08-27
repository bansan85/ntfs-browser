#include <ntfs-browser/file-reader.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{

FileReader::FileReader()
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle))
{
}

bool FileReader::Open(std::wstring_view volume)
{
  handle_ =
      HandlePtr(CreateFileW(volume.data(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr),
                &CloseHandle);
  return handle_.get() != INVALID_HANDLE_VALUE;
}

bool FileReader::Read(LARGE_INTEGER& addr, DWORD length, void* buf) const
{
  DWORD len = SetFilePointer(handle_.get(), static_cast<LONG>(addr.LowPart),
                             &addr.HighPart, FILE_BEGIN);

  if (len == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
  {
    NTFS_TRACE1("Cannot set file pointer to %I64d\n", addr.QuadPart);
    return false;
  }

  if (ReadFile(handle_.get(), buf, length, &len, nullptr) == FALSE ||
      len != length)
  {
    NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
    return false;
  }

  return true;
}

}  // namespace NtfsBrowser