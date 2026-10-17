#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

// Default size of one prefetched chunk: 4 MiB. A spinning disk reads that in
// a few milliseconds, which hides a seek and the hand-off between threads,
// and two chunks in flight still cost little memory.
inline constexpr std::uint64_t default_read_ahead_bytes = 4ULL * 1024 * 1024;

struct ReadAheadOptions {
  // Size of one prefetched chunk. Rounded up to a multiple of 4096, the
  // largest sector size NTFS supports, so that a raw device accepts the read.
  // A caller that asks for more than this per read gets chunks as large as its
  // reads, up to 64 MiB.
  std::uint64_t chunk_bytes{default_read_ahead_bytes};
};

// Decorates an IDiskReader with read-ahead. When the caller reads forward,
// each read starting where the previous one ended, a worker thread of its own
// reads the next two chunks while the caller works on the current one. Every
// other read goes straight to the wrapped reader. The wrapped reader MUST be
// random access: the worker reads at its own addresses. The wrapped reader is
// never called by two threads at once.
//
// Like the rest of the library, ReadInto() MUST be called by one thread at a
// time. The worker thread is internal: it shares no state with the caller
// beyond what this class guards. It is joined by the destructor, and the
// wrapped reader is not read again after that. The worker does not log. A read
// ahead that fails is dropped without a trace, and the caller's own read then
// reports the failure, on the caller's thread.
class NTFS_BROWSER_EXPORT ReadAheadDiskReader : public IDiskReader {
 public:
  // Takes ownership of the reader it decorates, already open or not.
  explicit ReadAheadDiskReader(std::unique_ptr<IDiskReader> inner,
                               const ReadAheadOptions& options = {});
  ~ReadAheadDiskReader() override;

  // Opens the wrapped reader, and forgets anything read ahead.
  bool Open(std::wstring_view path) override;

  // Reads from addr into dest, from the read ahead when it holds those bytes.
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
