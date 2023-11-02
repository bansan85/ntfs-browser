#include <cassert>

#include <ntfs-browser/file-reader.h>

#include "ntfs-common.h"

static constexpr LONGLONG READ_BUFFER_SIZE = 64 * 1024;
static constexpr LONGLONG MEMORY_BUFFER_SIZE = 512 * READ_BUFFER_SIZE;

namespace NtfsBrowser
{

template <Strategy S>
FileReader<S>::FileReader()
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle))
{
}

template <Strategy S>
bool FileReader<S>::Open(std::wstring_view volume)
{
  handle_ =
      HandlePtr(CreateFileW(volume.data(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr),
                &CloseHandle);
  return handle_.get() != INVALID_HANDLE_VALUE;
}

template <Strategy T>
template <Strategy Q>
typename std::enable_if_t<
    std::is_same_v<std::integral_constant<Strategy, Q>,
                   std::integral_constant<Strategy, Strategy::NO_CACHE>>,
    std::optional<std::span<const BYTE>>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const
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

  if (ReadFile(handle_.get(), buffer_.data(), length, &len, nullptr) == FALSE ||
      len != length)
  {
    NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
    return {};
  }

  return std::span<const BYTE>{buffer_.data(), length};
}

template <Strategy T>
template <Strategy Q>
typename std::enable_if_t<
    std::is_same_v<std::integral_constant<Strategy, Q>,
                   std::integral_constant<Strategy, Strategy::FULL_CACHE>>,
    std::optional<std::span<const BYTE>>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const
{
  // Not implemented. Really needed ?
  assert(addr.QuadPart / READ_BUFFER_SIZE ==
         (addr.QuadPart + length - 1) / READ_BUFFER_SIZE);

  size_t index = addr.QuadPart / READ_BUFFER_SIZE;
  if (map_buffer_.contains(index))
  {
    return std::span<const BYTE>{
        &map_buffer_[index][0] + addr.QuadPart % READ_BUFFER_SIZE, length};
  }

  LARGE_INTEGER addr2{.QuadPart =
                          addr.QuadPart - addr.QuadPart % READ_BUFFER_SIZE};
  DWORD len = SetFilePointer(handle_.get(), static_cast<LONG>(addr2.LowPart),
                             &addr2.HighPart, FILE_BEGIN);

  if (len == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
  {
    NTFS_TRACE1("Cannot set file pointer to %I64d\n", addr.QuadPart);
    return {};
  }

  BYTE* new_data = NextMemory();

  if (ReadFile(handle_.get(), &new_data[0], READ_BUFFER_SIZE, &len, nullptr) ==
          FALSE ||
      len != READ_BUFFER_SIZE)
  {
    NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
    return {};
  }

  map_buffer_[index] = new_data;

  return std::span<const BYTE>{new_data + addr.QuadPart % READ_BUFFER_SIZE,
                               length};
}

template <Strategy S>
BYTE* FileReader<S>::NextMemory() const
{
  if (mem_alloc.empty() || last_alloc * READ_BUFFER_SIZE == MEMORY_BUFFER_SIZE)
  {
    last_alloc = 0;
    mem_alloc.emplace_back(std::make_unique<BYTE[]>(MEMORY_BUFFER_SIZE));
  }
  BYTE* retval = &mem_alloc.back()[0] + last_alloc * READ_BUFFER_SIZE;
  last_alloc++;
  return retval;
}

template class FileReader<Strategy::NO_CACHE>;
template class FileReader<Strategy::FULL_CACHE>;

template std::optional<std::span<const BYTE>>
    FileReader<Strategy::NO_CACHE>::Read<Strategy::NO_CACHE>(
        LARGE_INTEGER& addr, DWORD length) const;
template std::optional<std::span<const BYTE>>
    FileReader<Strategy::FULL_CACHE>::Read<Strategy::FULL_CACHE>(
        LARGE_INTEGER& addr, DWORD length) const;

}  // namespace NtfsBrowser
