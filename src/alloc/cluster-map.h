#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "alloc/alloc-loader.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Alloc {

class BadClusterList;
class ClusterBitmap;

// What a volume knows about its clusters: the bitmap ($Bitmap) and the bad
// cluster list ($BadClus). Both load on first use through an AllocLoader. A
// failure is cached: every later query goes on without the file that failed,
// and logs once. Not thread safe.
template <Cache::Strategy S>
class ClusterMap {
 public:
  // cluster_size is the volume's, for the space summary. recover_errors only
  // decides whether a failure is logged as recoverable.
  ClusterMap(const AllocLoader<S>& loader, bool recover_errors,
             DWORD cluster_size);
  ~ClusterMap();

  ClusterMap(const ClusterMap&) = delete;
  ClusterMap& operator=(const ClusterMap&) = delete;
  ClusterMap(ClusterMap&&) = delete;
  ClusterMap& operator=(ClusterMap&&) = delete;

  // Keeps the bitmap windows read, up to max_bytes of them. The bitmap gets
  // the limit once loaded.
  void KeepWindows(ULONGLONG max_bytes);

  // Answers every query, as ClusterBitmap::StatesInOrder() does. A query
  // nobody answers stays unknown.
  void States(std::span<RangeQuery> queries);

  // The runs of one state over [lcn, lcn + clusters). Unknown over the whole
  // range when no bitmap answers.
  void Runs(ULONGLONG lcn, ULONGLONG clusters, std::vector<ClusterRun>& runs);

  // Free space over the whole volume, worked out once. Empty when no cluster
  // could be judged.
  [[nodiscard]] std::optional<SpaceSummary> Summary();

  // The bad clusters. Null when $BadClus is unusable.
  [[nodiscard]] const BadClusters* Bad();

 private:
  // The cluster bitmap, loaded now if it was not, and the bad list with it.
  // When only the bitmap fails, a bitmap with no bits stands in, so the
  // clusters the bad list names stay bad and the rest are unknown. Null when
  // nothing is usable, or while it loads.
  [[nodiscard]] ClusterBitmap* Bitmap();

  // The bad cluster list, loaded now if it was not. Null when it is unusable,
  // or while it loads.
  [[nodiscard]] const BadClusterList* BadList();

  // Logs, once per volume, that the bitmap turned out short or lost a window,
  // and, once per volume, that it calls free a cluster the bad list names.
  void NoteBitmapHealth();

  // keep_bytes_, cut to what this build can address.
  [[nodiscard]] size_t KeptBitmapBytes() const noexcept;

  AllocLoader<S> loader_;
  bool recover_errors_;
  DWORD cluster_size_;

  // The bad cluster list is a copy, so no record stays alive for it. It stays
  // null when $BadClus cannot be read; bad_loaded_ then keeps the failure from
  // being retried. Declared before the bitmap, which points to it, so it is
  // destroyed after it.
  std::unique_ptr<BadClusterList> bad_list_;

  // The record stays alive for as long as the bitmap reads through its data
  // stream, so it is declared first and destroyed last. bitmap_ stays null
  // when $Bitmap cannot be read; bitmap_loaded_ then keeps the failure from
  // being retried.
  std::unique_ptr<Io::FileRecord<S>> bitmap_record_;
  std::unique_ptr<ClusterBitmap> bitmap_;

  // Bytes of bitmap the caller asked to keep, which the bitmap gets once
  // loaded.
  ULONGLONG keep_bytes_{0};
  std::optional<SpaceSummary> summary_;

  bool bitmap_loaded_{false};
  // Set while the bitmap loads: a query made then answers Unknown instead of
  // loading it again.
  bool loading_bitmap_{false};
  // Set once a partial or unusable bitmap was logged: that log is once per
  // volume.
  bool degraded_logged_{false};
  bool summary_computed_{false};
  bool bad_loaded_{false};
  // Set while the bad list loads: a query made then sees no list instead of
  // loading it again.
  bool loading_bad_{false};
  // Set once a bad cluster that the bitmap calls free was logged: that log is
  // once per volume.
  bool bad_logged_{false};
};

}  // namespace Alloc
}  // namespace NTFS_BROWSER_NAMESPACE
