#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// Bytes of the cluster bitmap one window holds. One byte tracks 8 clusters, so
// a window of 1 MiB covers 8 Mi clusters: 32 GiB at 4 KiB per cluster. That is
// large enough to turn a sequential pass into one read per 32 GiB, and small
// enough to bound the memory a query uses whatever the volume size.
inline constexpr size_t default_window_bytes = size_t{1} * 1024 * 1024;

// Answers questions about the cluster bitmap of a volume, one window of it at
// a time. It does not know where the bits come from: a reader supplies them.
// An error narrows only the answer it touches. A window whose read fails or
// stops short keeps the bytes it returned for as long as it is held, marks the
// rest unknown, and is never read again. Not thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY ClusterBitmap {
 public:
  // Reads out.size() bytes of the bitmap at offset. Returns the byte count
  // read, or nothing on failure.
  using Reader = std::function<std::optional<ULONGLONG>(ULONGLONG offset,
                                                        std::span<BYTE> out)>;

  // How trustworthy the bitmap has proved so far.
  struct Status {
    // The bitmap holds fewer bits than the volume has clusters.
    bool shorter_than_volume{false};
    // Windows whose read failed or stopped short.
    ULONGLONG failed_windows{0};
  };

  // data_size is the bitmap's length in bytes. cluster_count is the volume's
  // cluster count, or 0 when unknown: the bitmap's own bit count then bounds
  // the queries.
  ClusterBitmap(Reader reader, ULONGLONG data_size, ULONGLONG cluster_count,
                size_t window_bytes = default_window_bytes);

  // State of one cluster.
  [[nodiscard]] ClusterState State(ULONGLONG lcn);

  // State of clusters [lcn, lcn + clusters). See Alloc::ClusterState.
  [[nodiscard]] ClusterState RangeState(ULONGLONG lcn, ULONGLONG clusters);

  // Answers every query. Queries are handled in LCN order, so each window is
  // read at most once, whatever the order of the array, which keeps its order.
  void StatesInOrder(std::span<RangeQuery> queries);

  // Free space over the whole volume. Empty when no cluster could be judged.
  // free_bytes saturates when its product with cluster_size overflows.
  [[nodiscard]] std::optional<SpaceSummary> Summary(DWORD cluster_size);

  [[nodiscard]] Status GetStatus() const noexcept;

  // Bytes of bitmap the window holds right now.
  [[nodiscard]] size_t BytesHeld() const noexcept;

 private:
  // Bits judged in one pass over a range.
  struct Counts {
    ULONGLONG allocated{0};
    ULONGLONG free{0};
    ULONGLONG unknown{0};
  };

  // The window held: bytes [index * window_bytes_, ...) of the bitmap.
  struct Window {
    ULONGLONG index{0};
    std::vector<BYTE> bytes;
    // Leading bytes the reader really returned.
    size_t valid{0};
  };

  // Judges bits [first, last), which lie inside the volume.
  [[nodiscard]] Counts Count(ULONGLONG first, ULONGLONG last);

  // The window of that index, read now if it is not the one held. Null when
  // that window failed earlier and is not held.
  [[nodiscard]] const Window* Load(ULONGLONG index);

  Reader reader_;
  ULONGLONG data_size_;
  // Bits the bitmap holds.
  ULONGLONG data_bits_;
  // Clusters the queries may name: the volume's, else the bitmap's own.
  ULONGLONG limit_;
  size_t window_bytes_;
  bool shorter_than_volume_;
  bool have_window_{false};
  Window window_;
  std::unordered_set<ULONGLONG> failed_;
};

// Test hook: the window size new bitmaps use. 0 restores the default. A test
// lowers it to span several windows with a small bitmap. It also resets the
// peak below.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetBitmapWindowBytesForTests(size_t bytes);

// The window size a new bitmap uses: the default, or the value a test set.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t BitmapWindowBytes();

// Test hook: the most bytes any bitmap has held since the last reset.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t PeakBitmapBytesHeld();

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
