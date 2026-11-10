#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// Reads the cluster bitmap one window at a time, and keeps some windows for
// later. It does not know what the bits mean: ClusterBitmap judges them. An
// error narrows only the window it touches. A window whose read fails or stops
// short keeps the bytes it returned for as long as it is held, and is never
// read again. Not thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY BitmapWindows {
 public:
  // Reads out.size() bytes of the bitmap at offset. Returns the byte count
  // read, or nothing on failure.
  using Reader = std::function<std::optional<ULONGLONG>(ULONGLONG offset,
                                                        std::span<BYTE> out)>;

  // The window held: bytes [index * WindowBytes(), ...) of the bitmap.
  struct Window {
    ULONGLONG index{0};
    std::vector<BYTE> bytes;
    // Leading bytes the reader really returned.
    size_t valid{0};
  };

  // data_size is the bitmap's length in bytes. window_bytes MUST NOT be 0.
  BitmapWindows(Reader reader, ULONGLONG data_size, size_t window_bytes);

  // Bytes of bitmap in one window.
  [[nodiscard]] size_t WindowBytes() const noexcept;

  // The window of that index, read now if it is not the one held. Null when
  // that window failed earlier and is not held, or when too many windows
  // failed to try another.
  [[nodiscard]] const Window* Load(ULONGLONG index);

  // Windows whose read failed or stopped short.
  [[nodiscard]] size_t FailedCount() const noexcept;

  // Whether so many windows failed that the rest of the bitmap is not worth
  // reading: a window that was not read is unknown without being tried.
  [[nodiscard]] bool GaveUp() const noexcept;

  // Bytes of bitmap held right now: the window being read, and the windows
  // kept by SetRetention().
  [[nodiscard]] size_t BytesHeld() const noexcept;

  // Keeps the windows read, up to max_bytes of them, for the queries that come
  // later, instead of reading a window again. Once the bytes are spent, the
  // other windows are read for one query each, as without retention: the kept
  // ones are the first read, which a sweep in LCN order meets again and again.
  // 0, the default, keeps nothing and frees what was kept.
  void SetRetention(size_t max_bytes);

 private:
  // Reads window index into window_, and keeps it in retained_ when the budget
  // allows. Returns the window to judge with.
  [[nodiscard]] const Window* ReadWindow(ULONGLONG index);

  Reader reader_;
  ULONGLONG data_size_;
  size_t window_bytes_;
  bool have_window_{false};
  Window window_;
  // The windows kept for later queries, by index, and their bytes. Never more
  // than retain_bytes_.
  std::map<ULONGLONG, Window> retained_;
  size_t retained_bytes_{0};
  size_t retain_bytes_{0};
  std::unordered_set<ULONGLONG> failed_;
};

// Clears the peak that PeakBytesHeld() reports.
void ResetPeakBytesHeld() noexcept;

// The most bytes any BitmapWindows has held since the last reset.
[[nodiscard]] size_t PeakBytesHeld() noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
