#include "alloc/cluster-map.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

#include "alloc/alloc-loader.h"
#include "alloc/bad-cluster-list.h"
#include "alloc/cluster-bitmap.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

template <Cache::Strategy S>
ClusterMap<S>::ClusterMap(const AllocLoader<S>& loader, bool recover_errors,
                          DWORD cluster_size)
    : loader_(loader),
      recover_errors_(recover_errors),
      cluster_size_(cluster_size) {}

template <Cache::Strategy S>
ClusterMap<S>::~ClusterMap() = default;

template <Cache::Strategy S>
void ClusterMap<S>::KeepWindows(ULONGLONG max_bytes) {
  keep_bytes_ = max_bytes;
  if (bitmap_) {
    bitmap_->SetRetention(KeptBitmapBytes());
  }
}

template <Cache::Strategy S>
void ClusterMap<S>::States(std::span<RangeQuery> queries) {
  // A query nobody answers stays unknown.
  for (RangeQuery& query : queries) {
    query.state = ClusterState::Unknown;
    query.allocated_clusters = 0;
    query.free_clusters = 0;
    query.bad_clusters = 0;
  }

  ClusterBitmap* bitmap = Bitmap();
  if (bitmap == nullptr) {
    return;
  }
  try {
    bitmap->StatesInOrder(queries);
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
  NoteBitmapHealth();
}

template <Cache::Strategy S>
void ClusterMap<S>::Runs(ULONGLONG lcn, ULONGLONG clusters,
                         std::vector<ClusterRun>& runs) {
  runs.clear();
  if (clusters == 0) {
    return;
  }
  ClusterBitmap* bitmap = Bitmap();
  if (bitmap != nullptr) {
    try {
      bitmap->StateRuns(lcn, clusters, runs);
      NoteBitmapHealth();
      return;
    } catch (const std::exception& e) {
      Log::Exception(e);
    }
  }
  // A query nobody answers stays unknown.
  runs.assign(
      1, {.lcn = lcn, .clusters = clusters, .state = ClusterState::Unknown});
}

template <Cache::Strategy S>
std::optional<SpaceSummary> ClusterMap<S>::Summary() {
  if (!summary_computed_) {
    summary_computed_ = true;
    ClusterBitmap* bitmap = Bitmap();
    if (bitmap != nullptr) {
      try {
        summary_ = bitmap->Summary(cluster_size_);
      } catch (const std::exception& e) {
        Log::Exception(e);
      }
      NoteBitmapHealth();
    }
  }
  return summary_;
}

template <Cache::Strategy S>
const BadClusters* ClusterMap<S>::Bad() {
  try {
    const BadClusterList* list = BadList();
    return list != nullptr ? &list->Get() : nullptr;
  } catch (const std::exception& e) {
    Log::Exception(e);
    return nullptr;
  }
}

template <Cache::Strategy S>
const BadClusterList* ClusterMap<S>::BadList() {
  if (loading_bad_) {
    return nullptr;
  }
  if (!bad_loaded_) {
    bad_loaded_ = true;
    loading_bad_ = true;
    try {
      bad_list_ = loader_.LoadBadList();
    } catch (const std::exception& e) {
      Log::Exception(e);
      bad_list_.reset();
    }
    loading_bad_ = false;
    if (!bad_list_) {
      Log::Recoverable(recover_errors_,
                       "$BadClus is not usable: bad clusters are unknown");
    }
  }
  return bad_list_.get();
}

template <Cache::Strategy S>
ClusterBitmap* ClusterMap<S>::Bitmap() {
  if (loading_bitmap_) {
    return nullptr;
  }
  if (!bitmap_loaded_) {
    bitmap_loaded_ = true;
    const BadClusterList* bad = BadList();
    loading_bitmap_ = true;
    try {
      LoadedBitmap<S> loaded = loader_.LoadBitmap();
      bitmap_ = std::move(loaded.bitmap);
      bitmap_record_ = std::move(loaded.record);
    } catch (const std::exception& e) {
      Log::Exception(e);
      bitmap_.reset();
    }
    loading_bitmap_ = false;
    if (!bitmap_) {
      degraded_logged_ = true;
      Log::Recoverable(recover_errors_,
                       "$Bitmap is not usable: cluster allocation is unknown");
      const ULONGLONG cluster_count = loader_.UsableClusterCount();
      if (bad != nullptr && !bad->Get().extents.empty() && cluster_count != 0) {
        bitmap_ = loader_.MakeUnknownBitmap(cluster_count);
      }
    }
    if (bitmap_) {
      bitmap_->SetBadList(bad);
      bitmap_->SetRetention(KeptBitmapBytes());
    }
  }
  return bitmap_.get();
}

template <Cache::Strategy S>
void ClusterMap<S>::NoteBitmapHealth() {
  if (bitmap_ && !bad_logged_ && bitmap_->GetStatus().bad_with_clear_bit) {
    bad_logged_ = true;
    Log::Recoverable(recover_errors_,
                     "Clusters listed in $BadClus are free in $Bitmap: they "
                     "are bad all the same");
  }
  if (!bitmap_ || degraded_logged_) {
    return;
  }
  const ClusterBitmap::Status status = bitmap_->GetStatus();
  if (status.shorter_than_volume || status.failed_windows != 0) {
    degraded_logged_ = true;
    Log::Recoverable(recover_errors_,
                     "$Bitmap is {}: some clusters are unknown",
                     status.shorter_than_volume ? "shorter than the volume"
                                                : "partly unreadable");
  }
}

template <Cache::Strategy S>
size_t ClusterMap<S>::KeptBitmapBytes() const noexcept {
  return static_cast<size_t>(
      (std::min<ULONGLONG>)(keep_bytes_, (std::numeric_limits<size_t>::max)()));
}

template class ClusterMap<Cache::Strategy::NoCache>;
template class ClusterMap<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
