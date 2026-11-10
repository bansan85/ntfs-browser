#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "disk-reader.h"
#include "internal-export.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// An IDiskReader that two threads share, one call at a time. The wrapped reader
// may keep a file position, so it is called under a mutex of its own: by the
// worker of a ReadAheadDiskReader for a chunk, by the caller for a direct read.
// A call on a missing reader does nothing, and a read of it fails.
class NTFS_BROWSER_EXPORT_TESTS_ONLY GuardedDiskReader {
 public:
  // Takes ownership of the reader, already open or not. It may be null.
  explicit GuardedDiskReader(std::unique_ptr<IDiskReader> inner)
      : inner_(std::move(inner)) {}

  GuardedDiskReader(const GuardedDiskReader&) = delete;
  GuardedDiskReader& operator=(const GuardedDiskReader&) = delete;
  GuardedDiskReader(GuardedDiskReader&&) = delete;
  GuardedDiskReader& operator=(GuardedDiskReader&&) = delete;
  ~GuardedDiskReader() = default;

  // Opens the wrapped reader. False when there is none.
  bool Open(std::wstring_view path);

  // Reads dest.size() bytes at addr on the calling thread. False when the
  // wrapped reader is missing or fails.
  [[nodiscard]] bool Read(LONGLONG addr, std::span<BYTE> dest);

  // Reads like Read(), for a thread of its own: false at once when cancelled
  // is set by the time the wrapped reader is free, and an exception ends as a
  // failure. The caller's direct read meets it again, and reports it there.
  [[nodiscard]] bool ReadUnlessCancelled(const std::atomic<bool>& cancelled,
                                         LONGLONG addr, std::span<BYTE> dest);

  // Size of the wrapped reader. It reads nothing.
  [[nodiscard]] std::optional<ULONGLONG> Size();

  // Kind of device of the wrapped reader. It reads nothing.
  [[nodiscard]] Tuning::MediaKind GetMediaKind();

  // Pass the hints of the caller on to the wrapped reader, which the worker may
  // be reading.
  void Prefetch(ULONGLONG offset, ULONGLONG length);
  void CancelPrefetch();
  void PrefetchStanding(ULONGLONG offset, ULONGLONG length);
  void CancelStanding(ULONGLONG offset, ULONGLONG length);

  // Passes the read pattern on to the wrapped reader.
  void SetReadPattern(Tuning::Pattern pattern);

 private:
  // Held by whoever calls the wrapped reader.
  std::mutex mutex_;
  std::unique_ptr<IDiskReader> inner_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
