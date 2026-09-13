#include <cassert>

#include <ntfs-browser/file-reader.h>
#include <ntfs-browser/win32-disk-reader.h>

#include "ntfs-common.h"

static constexpr LONGLONG READ_BUFFER_SIZE = 64 * 1024;
static constexpr LONGLONG MEMORY_BUFFER_SIZE = 512 * READ_BUFFER_SIZE;

namespace NtfsBrowser
{

template <Strategy S>
FileReader<S>::FileReader() = default;

template <Strategy S>
FileReader<S>::FileReader(std::unique_ptr<IDiskReader> reader)
    : reader_(std::move(reader))
{
}

template <Strategy S>
bool FileReader<S>::Open(std::wstring_view volume)
{
  auto reader = std::make_unique<Win32DiskReader>();
  if (!reader->Open(volume))
  {
    return false;
  }

  reader_ = std::move(reader);
  return true;
}

template <Strategy S>
bool FileReader<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const
{
  return reader_->ReadInto(addr, dest);
}

template <Strategy T>
template <Strategy Q>
typename std::enable_if_t<
    std::is_same_v<std::integral_constant<Strategy, Q>,
                   std::integral_constant<Strategy, Strategy::NO_CACHE>>,
    std::optional<std::span<const BYTE>>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const
{
  if (buffer_.capacity() < length)
  {
    buffer_.reserve(length);
  }

  if (!reader_->ReadInto(addr, std::span<BYTE>{buffer_.data(), length}))
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

  BYTE* new_data = NextMemory();

  if (!reader_->ReadInto(
          addr2,
          std::span<BYTE>{new_data, static_cast<size_t>(READ_BUFFER_SIZE)}))
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
