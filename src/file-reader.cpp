#include <cassert>

#include <ntfs-browser/file-reader.h>

#include "ntfs-common.h"

static constexpr LONGLONG BUFFER_SIZE = 32 * 1024;

namespace NtfsBrowser
{

FileReader::FileReader(Strategy strategy)
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle)),
      strategy_(strategy)
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

std::optional<std::span<const BYTE>> FileReader::Read(LARGE_INTEGER& addr,
                                                      DWORD length) const
{
  if (strategy_ == Strategy::NO_CACHE)
  {
    DWORD len = SetFilePointer(handle_.get(), static_cast<LONG>(addr.LowPart),
                               &addr.HighPart, FILE_BEGIN);

    if (len == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
    {
      NTFS_TRACE1("Cannot set file pointer to %I64d\n", addr.QuadPart);
      return {};
    }

    if (buffer_.capacity() < length)
    {
      buffer_.reserve(length);
    }

    if (ReadFile(handle_.get(), buffer_.data(), length, &len, nullptr) ==
            FALSE ||
        len != length)
    {
      NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
      return {};
    }

    return std::span<const BYTE>{buffer_.data(), length};
  }
  if (strategy_ == Strategy::FULL_CACHE)
  {
    // Not implemented. Really needed ?
    assert(addr.QuadPart / BUFFER_SIZE ==
           (addr.QuadPart + length - 1) / BUFFER_SIZE);

    size_t index = addr.QuadPart / BUFFER_SIZE;
    if (map_buffer_.contains(index))
    {
      return std::span<const BYTE>{
          &map_buffer_[index][addr.QuadPart % BUFFER_SIZE], length};
    }

    LARGE_INTEGER addr2{.QuadPart =
                            addr.QuadPart - addr.QuadPart % BUFFER_SIZE};
    DWORD len = SetFilePointer(handle_.get(), static_cast<LONG>(addr2.LowPart),
                               &addr2.HighPart, FILE_BEGIN);

    if (len == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
    {
      NTFS_TRACE1("Cannot set file pointer to %I64d\n", addr.QuadPart);
      return {};
    }

    std::vector<BYTE> new_vector;
    new_vector.reserve(BUFFER_SIZE);
    map_buffer_[index] = std::move(new_vector);

    std::vector<BYTE>& map_buf = map_buffer_[index];

    if (ReadFile(handle_.get(), &map_buf[0], BUFFER_SIZE, &len, nullptr) ==
            FALSE ||
        len != BUFFER_SIZE)
    {
      NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
      return {};
    }

    return std::span<const BYTE>{&map_buf[addr.QuadPart % BUFFER_SIZE], length};
  }
  return {};
}

FileReader::Strategy FileReader::GetStrategy() const { return strategy_; }

}  // namespace NtfsBrowser