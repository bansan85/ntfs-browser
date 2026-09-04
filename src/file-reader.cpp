#include <algorithm>
#include <cstring>

#include <ntfs-browser/file-reader.h>

#include "ntfs-common.h"

#ifdef _WIN32
  #include <ntfs-browser/win32-disk-reader.h>
#endif

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

#ifdef _WIN32
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
#endif

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

// Fetches (loading and caching on first access) the single 64KiB block
// containing "blockAddr" - which must already be 64KiB-aligned - and
// returns a pointer to its start, or nullptr on a read failure.
template <Strategy S>
BYTE* FileReader<S>::GetCachedBlock(LARGE_INTEGER blockAddr) const
{
  const size_t index = blockAddr.QuadPart / READ_BUFFER_SIZE;
  const auto it = map_buffer_.find(index);
  if (it != map_buffer_.end())
  {
    return it->second;
  }

  BYTE* new_data = NextMemory();

  if (!reader_->ReadInto(
          blockAddr,
          std::span<BYTE>{new_data, static_cast<size_t>(READ_BUFFER_SIZE)}))
  {
    return nullptr;
  }

  map_buffer_[index] = new_data;
  return new_data;
}

// F19 fix: NextMemory() only ever hands out 64KiB-sized slices, carved out
// of separate 512*64KiB (32MiB) make_unique<BYTE[]> allocations - a read
// spanning more than one such slice cannot be returned as a single
// zero-copy view, since neighbouring slices aren't guaranteed to be
// adjacent in memory (they can even belong to different 32MiB
// allocations). The previous implementation didn't handle that case at
// all: it just assert()-ed that [addr, addr + length) never crosses a
// 64KiB boundary - a check compiled out entirely in Release builds
// (NDEBUG) - and, should a crossing request ever reach it regardless,
// returned a span reaching past the single 64KiB slice it actually read,
// up to and including past the end of the 32MiB allocation backing it (see
// bug F19 in docs/bug-reports/2026-09-03-full-repo.md for the full
// attacker-controlled path via a forged cluster_size_, and
// file-reader-full-cache-boundary-tests.cpp for the regression test).
//
// A crossing read isn't just a malicious-input concern either: cluster
// size mostly determines the addr/length here (see
// AttrNonResident<S>::ReadClusters() in attr-non-resident.cpp), and even a
// perfectly ordinary volume with a real-world cluster size can trivially
// exceed 64KiB, or straddle a 64KiB boundary from a non-block-aligned LCN,
// on any sufficiently large contiguous cluster run - nothing here is
// specific to a corrupted BPB.
//
// Rather than rejecting crossing reads outright, or trying to enforce
// alignment further up the call chain (which would still leave this
// function relying on every caller getting it right), loop over as many
// 64KiB blocks as the request actually needs and stitch the result
// together into its own dedicated allocation. That allocation is kept
// alive in crossing_reads_ for this FileReader's lifetime, matching
// FULL_CACHE's existing contract that a previously returned span stays
// valid indefinitely, not just until the next Read() call (unlike
// NO_CACHE's short-lived buffer_).
template <Strategy T>
template <Strategy Q>
typename std::enable_if_t<
    std::is_same_v<std::integral_constant<Strategy, Q>,
                   std::integral_constant<Strategy, Strategy::FULL_CACHE>>,
    std::optional<std::span<const BYTE>>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const
{
  if (length == 0)
  {
    return std::span<const BYTE>{};
  }

  const bool crossesBlock = addr.QuadPart / READ_BUFFER_SIZE !=
                            (addr.QuadPart + length - 1) / READ_BUFFER_SIZE;

  if (!crossesBlock)
  {
    // Fast path (the overwhelmingly common case): the whole request lives
    // in a single 64KiB cache block. Zero-copy view straight into
    // long-lived cached memory, exactly as before this fix.
    const LARGE_INTEGER blockAddr{.QuadPart = addr.QuadPart -
                                              addr.QuadPart % READ_BUFFER_SIZE};
    BYTE* block = GetCachedBlock(blockAddr);
    if (block == nullptr)
    {
      NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
      return {};
    }

    return std::span<const BYTE>{block + addr.QuadPart % READ_BUFFER_SIZE,
                                 length};
  }

  // Slow path: stitch the requested range together, one 64KiB block at a
  // time, into its own allocation.
  auto assembled = std::make_unique<BYTE[]>(length);
  BYTE* const result = assembled.get();
  BYTE* out = result;

  LARGE_INTEGER cur = addr;
  DWORD remaining = length;
  while (remaining != 0)
  {
    const LARGE_INTEGER blockAddr{.QuadPart = cur.QuadPart -
                                              cur.QuadPart % READ_BUFFER_SIZE};
    BYTE* block = GetCachedBlock(blockAddr);
    if (block == nullptr)
    {
      NTFS_TRACE1("Cannot read file at adress %I64d\n", addr.QuadPart);
      return {};
    }

    const auto offsetInBlock =
        static_cast<DWORD>(cur.QuadPart % READ_BUFFER_SIZE);
    const DWORD chunk = static_cast<DWORD>(
        std::min<LONGLONG>(READ_BUFFER_SIZE - offsetInBlock, remaining));

    memcpy(out, block + offsetInBlock, chunk);

    out += chunk;
    cur.QuadPart += chunk;
    remaining -= chunk;
  }

  crossing_reads_.push_back(std::move(assembled));
  return std::span<const BYTE>{result, length};
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
