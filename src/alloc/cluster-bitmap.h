#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/bitmap-windows.h"
#include "alloc/types.h"
#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

class BadClusterList;

// Bytes of the cluster bitmap one window holds. One byte tracks 8 clusters, so
// a window of 1 MiB covers 8 Mi clusters: 32 GiB at 4 KiB per cluster. That is
// large enough to turn a sequential pass into one read per 32 GiB, and small
// enough to bound the memory a query uses whatever the volume size.
inline constexpr size_t default_window_bytes = size_t{1} * 1024 * 1024;

// Answers questions about the cluster bitmap of a volume, one window of it at
// a time. It does not know where the bits come from: a reader supplies them.
// An error narrows only the answer it touches. A window whose read fails or
// stops short keeps the bytes it returned for as long as it is held, marks the
// rest unknown, and is never read again. A bad cluster list, when attached,
// takes its clusters out of the allocated, free and unknown counts: they are
// bad, whatever the bitmap says of them. Not thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY ClusterBitmap {
 public:
  // Reads out.size() bytes of the bitmap at offset. Returns the byte count
  // read, or nothing on failure.
  using Reader = BitmapWindows::Reader;

  // How trustworthy the bitmap has proved so far.
  struct Status {
    // The bitmap holds fewer bits than the volume has clusters.
    bool shorter_than_volume{false};
    // Windows whose read failed or stopped short.
    ULONGLONG failed_windows{0};
    // A cluster the bad list names has its bit clear in the bitmap: the two
    // files disagree. Set once a query has met one.
    bool bad_with_clear_bit{false};
  };

  // data_size is the bitmap's length in bytes. cluster_count is the volume's
  // cluster count, or 0 when unknown: the bitmap's own bit count then bounds
  // the queries.
  ClusterBitmap(Reader reader, ULONGLONG data_size, ULONGLONG cluster_count,
                size_t window_bytes = default_window_bytes);

  // Attaches the bad cluster list. The list MUST outlive this object, and is
  // set once, before the first query.
  void SetBadList(const BadClusterList* list) noexcept;

  // State of one cluster.
  [[nodiscard]] ClusterState State(ULONGLONG lcn);

  // State of clusters [lcn, lcn + clusters). See Alloc::ClusterState.
  [[nodiscard]] ClusterState RangeState(ULONGLONG lcn, ULONGLONG clusters);

  // Answers every query. Queries are handled in LCN order, so each window is
  // read at most once, whatever the order of the array, which keeps its order.
  void StatesInOrder(std::span<RangeQuery> queries);

  // The state of every cluster of [lcn, lcn + clusters), as maximal runs of one
  // state, in LCN order: Allocated, Free, Unknown (not judged), Bad (the bad
  // list wins over the bit) and OutOfRange (past the volume). An empty range
  // gives no run. Handled like a query of StatesInOrder(): a window is read
  // when the range reaches it, and kept as that says.
  void StateRuns(ULONGLONG lcn, ULONGLONG clusters,
                 std::vector<ClusterRun>& runs);

  // Free space over the whole volume. Empty when no cluster could be judged.
  // free_bytes saturates when its product with cluster_size overflows.
  [[nodiscard]] std::optional<SpaceSummary> Summary(DWORD cluster_size);

  [[nodiscard]] Status GetStatus() const noexcept;

  // Bytes of bitmap held right now: the window being read, and the windows
  // kept by SetRetention().
  [[nodiscard]] size_t BytesHeld() const noexcept;

  // Keeps the windows read, up to max_bytes of them, for the queries that come
  // later. See BitmapWindows::SetRetention().
  void SetRetention(size_t max_bytes);

 private:
  // Clusters judged in one pass over a range. allocated, free, unknown and bad
  // never overlap. judged counts the clusters whose bit was read, bad ones
  // included.
  struct Counts {
    ULONGLONG allocated{0};
    ULONGLONG free{0};
    ULONGLONG unknown{0};
    ULONGLONG bad{0};
    ULONGLONG judged{0};
  };

  using Window = BitmapWindows::Window;

  // The part of a range whose bits were read: bits [.., stop) come from
  // window, whose first bit is base. The rest of the range was not judged, and
  // window may be null then.
  struct JudgedBits {
    const Window* window{nullptr};
    ULONGLONG base{0};
    ULONGLONG stop{0};
  };

  // The part of a range that lies in one window: the bits read, and where the
  // window ends.
  struct Slice {
    JudgedBits judged;
    // End of the part of the range this window covers, judged or not.
    ULONGLONG stop{0};
  };

  // Appends the runs of bits [first, last) of the bitmap, bad list left out.
  // The range lies inside the volume.
  void BitRuns(ULONGLONG first, ULONGLONG last, std::vector<ClusterRun>& runs);

  // Replaces the runs of [first, last) the bad list names by Bad ones.
  void OverlayBad(std::vector<ClusterRun>& runs);

  // Judges bits [first, last), which lie inside the volume.
  [[nodiscard]] Counts Count(ULONGLONG first, ULONGLONG last);

  // Loads the window that holds bit, and tells how much of [bit, last) it
  // judges. bit MUST lie inside the bitmap.
  [[nodiscard]] Slice ReadSlice(ULONGLONG bit, ULONGLONG last);

  // Counts clusters [first, last) as unknown, bad ones apart, without reading
  // any window.
  void CountUnknown(Counts& counts, ULONGLONG first, ULONGLONG last);

  // Moves the clusters of [first, last) that the bad list names into
  // counts.bad. judged says which of those bits were read.
  void ApplyBad(Counts& counts, ULONGLONG first, ULONGLONG last,
                const JudgedBits& judged);

  BitmapWindows windows_;
  // Bits the bitmap holds.
  ULONGLONG data_bits_;
  // Clusters the queries may name: the volume's, else the bitmap's own.
  ULONGLONG limit_;
  bool shorter_than_volume_;
  const BadClusterList* bad_{nullptr};
  bool bad_with_clear_bit_{false};
};

// Test hook: the window size new bitmaps use. 0 restores the default. A test
// lowers it to span several windows with a small bitmap. It also resets the
// peak below.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetBitmapWindowBytesForTests(size_t bytes);

// The window size a new bitmap uses: the default, or the value a test set.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t BitmapWindowBytes();

// Same, with tuned as the default: the window that VolumeOptions::tuning asks
// for, unless a test set one.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t
    BitmapWindowBytes(size_t tuned);

// Test hook: the most bytes any bitmap has held since the last reset.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t PeakBitmapBytesHeld();

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
