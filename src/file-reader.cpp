#include "file-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include "internal-export.h"
#include "ntfs-common.h"

#ifdef _WIN32
  #include <string_view>

  #include "win32-disk-reader.h"
#endif

namespace NtfsBrowser::Io {

namespace {

// Size of one block read from the disk and cached. 64 KiB amortises the cost
// of a disk read without holding much more than a cluster run's worth of data.
constexpr LONGLONG read_buffer_size = LONGLONG{64} * 1024;

// Size of one allocation that backs many blocks: 512 blocks, so 32 MiB.
constexpr LONGLONG memory_buffer_size = 512 * read_buffer_size;

// read_buffer_size as a size_t, to size a std::span over one cached block.
constexpr size_t block_bytes_value = static_cast<size_t>(read_buffer_size);

}  // namespace

template <Cache::Strategy S>
FileReader<S>::FileReader() = default;

template <Cache::Strategy S>
FileReader<S>::FileReader(std::unique_ptr<IDiskReader> reader)
    : reader_(std::move(reader)) {}

#ifdef _WIN32
template <Cache::Strategy S>
bool FileReader<S>::Open(std::wstring_view volume) {
  auto reader = std::make_unique<Win32DiskReader>();
  if (!reader->Open(volume)) {
    return false;
  }

  reader_ = std::move(reader);
  return true;
}
#endif

template <Cache::Strategy S>
bool FileReader<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const {
  return reader_->ReadInto(addr, dest);
}

template <Cache::Strategy T>
template <Cache::Strategy Q>
std::enable_if_t<std::is_same_v<std::integral_constant<Cache::Strategy, Q>,
                                std::integral_constant<
                                    Cache::Strategy, Cache::Strategy::NoCache>>,
                 std::optional<std::span<const BYTE>>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const {
  if (buffer_.size() < length) {
    buffer_.resize(length);
  }

  if (!reader_->ReadInto(addr, std::span<BYTE>{buffer_.data(), length})) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return {};
  }

  return std::span<const BYTE>{buffer_.data(), length};
}

// Fetches (loading and caching on first access) the single 64KiB block
// containing "blockAddr" - which must already be 64KiB-aligned - and
// returns a pointer to its start, or nullptr on a read failure.
template <Cache::Strategy S>
BYTE* FileReader<S>::GetCachedBlock(LARGE_INTEGER block_addr) const {
  const size_t index = block_addr.QuadPart / read_buffer_size;
  const auto iterator = map_buffer_.find(index);
  if (iterator != map_buffer_.end()) {
    return iterator->second;
  }

  BYTE* new_data = NextMemory();

  if (!reader_->ReadInto(
          block_addr,
          std::span<BYTE>{new_data, static_cast<size_t>(read_buffer_size)})) {
    return nullptr;
  }

  map_buffer_[index] = new_data;
  return new_data;
}

// Reads exactly "length" bytes at "addr" into a buffer this reader owns,
// bypassing the block cache. FullCache falls back to it when a whole 64KiB
// block cannot be read: the block may extend past the end of the medium. The
// short block MUST NOT be cached as if it were complete.
template <Cache::Strategy S>
std::optional<std::span<const BYTE>>
    FileReader<S>::ReadUncached(LARGE_INTEGER addr, DWORD length) const {
  std::vector<BYTE> exact(length);
  if (!reader_->ReadInto(addr, exact)) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return {};
  }

  const BYTE* const data = exact.data();
  crossing_reads_.push_back(std::move(exact));
  return std::span<const BYTE>{data, length};
}

template <Cache::Strategy T>
template <Cache::Strategy Q>
std::enable_if_t<
    std::is_same_v<
        std::integral_constant<Cache::Strategy, Q>,
        std::integral_constant<Cache::Strategy, Cache::Strategy::FullCache>>,
    std::optional<std::span<const BYTE>>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const {
  if (length == 0) {
    return std::span<const BYTE>{};
  }

  // A negative address has no block to cache. An end past LLONG_MAX would
  // overflow the block arithmetic below.
  if (addr.QuadPart < 0 ||
      addr.QuadPart > std::numeric_limits<LONGLONG>::max() - length) {
    Log::Error("Cannot read file at adress {}: range is out of bounds",
               addr.QuadPart);
    return {};
  }

  const bool crosses_block = addr.QuadPart / read_buffer_size !=
                             (addr.QuadPart + length - 1) / read_buffer_size;

  if (!crosses_block) {
    // Fast path: the request fits in a single block; return a zero-copy view.
    const LARGE_INTEGER block_addr{
        .QuadPart = addr.QuadPart - addr.QuadPart % read_buffer_size};
    BYTE* block = GetCachedBlock(block_addr);
    if (block == nullptr) {
      return ReadUncached(addr, length);
    }

    return std::span<const BYTE>{block, block_bytes_value}.subspan(
        gsl::narrow<size_t>(addr.QuadPart % read_buffer_size), length);
  }

  // Slow path: stitch the range together one block at a time.
  std::vector<BYTE> assembled(length);
  BYTE* const result = assembled.data();
  std::span<BYTE> out{result, length};

  LARGE_INTEGER cur = addr;
  DWORD remaining = length;
  while (remaining != 0) {
    const LARGE_INTEGER block_addr{.QuadPart = cur.QuadPart -
                                               cur.QuadPart % read_buffer_size};
    BYTE const* block = GetCachedBlock(block_addr);
    if (block == nullptr) {
      return ReadUncached(addr, length);
    }

    const auto offset_in_block =
        gsl::narrow<DWORD>(cur.QuadPart % read_buffer_size);
    const auto chunk = gsl::narrow<DWORD>(
        std::min<LONGLONG>(read_buffer_size - offset_in_block, remaining));

    const std::span<const BYTE> block_bytes{block, block_bytes_value};
    // offsetInBlock is a remainder modulo READ_BUFFER_SIZE = block_bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    memcpy(out.data(), &block_bytes[offset_in_block], chunk);

    out = out.subspan(chunk);
    cur.QuadPart += chunk;
    remaining -= chunk;
  }

  crossing_reads_.push_back(std::move(assembled));
  return std::span<const BYTE>{result, length};
}

template <Cache::Strategy S>
BYTE* FileReader<S>::NextMemory() const {
  if (mem_alloc_.empty() ||
      last_alloc_ * read_buffer_size == memory_buffer_size) {
    last_alloc_ = 0;
    mem_alloc_.emplace_back(memory_buffer_size);
  }
  // last_alloc was reset above once the buffer held MEMORY_BUFFER_SIZE bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  BYTE* retval = &mem_alloc_.back()[last_alloc_ * block_bytes_value];
  last_alloc_++;
  return retval;
}

template class FileReader<Cache::Strategy::NoCache>;
template class FileReader<Cache::Strategy::FullCache>;

// Class-level NTFS_BROWSER_EXPORT_TESTS_ONLY (on FileReader) does not reach a
// member function template's own explicit instantiations: each needs the
// macro again here, or the unit tests cannot link against it on a shared
// build.
template NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<std::span<const BYTE>>
    FileReader<Cache::Strategy::NoCache>::Read<Cache::Strategy::NoCache>(
        LARGE_INTEGER& addr, DWORD length) const;
template NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<std::span<const BYTE>>
    FileReader<Cache::Strategy::FullCache>::Read<Cache::Strategy::FullCache>(
        LARGE_INTEGER& addr, DWORD length) const;

}  // namespace NtfsBrowser::Io
