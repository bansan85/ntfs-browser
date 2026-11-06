#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "alloc/bad-cluster-list.h"
#include "alloc/cluster-bitmap.h"
#include "bench/recorder.h"
#include "io/file-reader.h"
#include "log/ntfs-common.h"
#include "ntfs-volume-impl.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace {

// Reads out.size() bytes of a bitmap stream at offset. When the whole read
// fails or stops short, it reads again piece by piece, so the pieces before the
// damage are not lost with it. Returns the bytes read from the start, or
// nothing when not even the first piece could be read. Never throws.
template <Cache::Strategy S>
std::optional<ULONGLONG> ReadBitmapBytes(const Attr::AttrBase<S>& stream,
                                         DWORD piece, ULONGLONG offset,
                                         std::span<BYTE> out) {
  const Bench::Detail::KindScope kind(Bench::ReadKind::BitmapWindow);
  try {
    const std::optional<ULONGLONG> whole = stream.ReadData(offset, out);
    if (whole && *whole == out.size()) {
      return whole;
    }

    ULONGLONG done = 0;
    while (done < out.size()) {
      const size_t length =
          static_cast<size_t>(std::min<ULONGLONG>(piece, out.size() - done));
      const std::optional<ULONGLONG> part =
          stream.ReadData(offset + done, out.subspan(done, length));
      if (!part || *part == 0) {
        break;
      }
      done += *part;
      if (*part < length) {
        break;
      }
    }
    if (done == 0) {
      return std::nullopt;
    }
    return done;
  } catch (const std::exception& e) {
    Log::Exception(e);
    return std::nullopt;
  }
}

}  // namespace

// Reads $Bitmap (MFT record 6) and wraps its data stream. Null when the record
// or the stream is missing or unusable. Like the volume's other metadata reads,
// it MUST NOT depend on include_deleted. With recover_errors off a damaged run
// list rejects the attribute, so the bitmap is absent; on, the runs that parse
// are kept, and the clusters past them read as unknown.
template <Cache::Strategy S>
std::unique_ptr<Alloc::ClusterBitmap>
    NtfsVolume<S>::Impl::LoadClusterBitmap() const {
  const auto bitmap_record_idx = static_cast<ULONGLONG>(Mft::Idx::Bitmap);
  if (!IsMetadataMapped(bitmap_record_idx)) {
    return {};
  }

  auto record = std::make_unique<Io::FileRecord<S>>(*self);
  if (!ParseMetadataData(*record, bitmap_record_idx)) {
    return {};
  }

  const Attr::AttrBase<S>* data = record->FindStream({});
  if (data == nullptr) {
    return {};
  }

  // The store cannot hold more clusters than its size says, and a volume that
  // claims more is cut at the store. Bytes of bitmap past what those clusters
  // need are never looked at, so a forged size cannot make a query long.
  ULONGLONG cluster_count = geometry.cluster_count;
  ULONGLONG data_size = data->GetDataSize();
  if (const std::optional<ULONGLONG> store_bytes = volume->Size();
      store_bytes && geometry.cluster_size != 0) {
    const ULONGLONG store_clusters = *store_bytes / geometry.cluster_size;
    cluster_count =
        cluster_count == 0 ? 0 : std::min(cluster_count, store_clusters);
    data_size = std::min(data_size, store_clusters / 8 + 1);
  }
  if (cluster_count != 0) {
    data_size = std::min(data_size, cluster_count / 8 + 1);
  }

  const DWORD piece = geometry.cluster_size;
  auto reader = [data, piece](ULONGLONG offset, std::span<BYTE> out) {
    return ReadBitmapBytes<S>(*data, piece, offset, out);
  };
  auto loaded = std::make_unique<Alloc::ClusterBitmap>(
      reader, data_size, cluster_count,
      Alloc::BitmapWindowBytes(options.tuning.alloc.bitmap_window_bytes));
  bitmap_record = std::move(record);
  return loaded;
}

// The volume's cluster count, cut where the backing store ends: a volume that
// claims more clusters than its store can hold is cut at the store.
template <Cache::Strategy S>
ULONGLONG NtfsVolume<S>::Impl::UsableClusterCount() const {
  ULONGLONG cluster_count = geometry.cluster_count;
  if (const std::optional<ULONGLONG> store_bytes = volume->Size();
      store_bytes && geometry.cluster_size != 0 && cluster_count != 0) {
    cluster_count =
        std::min(cluster_count, *store_bytes / geometry.cluster_size);
  }
  return cluster_count;
}

// Reads $BadClus (MFT record 8) and copies the real runs of its $Bad stream.
// Null when the record or the stream is missing or unusable, or when the runs
// are damaged and recover_errors is off. Like the volume's other metadata
// reads, it MUST NOT depend on include_deleted. The record is local: once the
// extents are copied, its run list is released.
template <Cache::Strategy S>
std::unique_ptr<Alloc::BadClusterList>
    NtfsVolume<S>::Impl::LoadBadClusterList() const {
  const auto bad_record_idx = static_cast<ULONGLONG>(Mft::Idx::BadCluster);
  if (!IsMetadataMapped(bad_record_idx)) {
    return {};
  }

  Io::FileRecord<S> record(*self);
  if (!ParseMetadataData(record, bad_record_idx)) {
    return {};
  }

  const Attr::AttrBase<S>* stream = record.FindStream(L"$Bad");
  if (stream == nullptr) {
    return {};
  }
  return Alloc::BadClusterList::Build(
      stream->GetExtents(), UsableClusterCount(), options.recover_errors);
}

// Loads the bad cluster list on first use. A failure is cached: every later
// query goes on without the list, and the bitmap alone answers.
template <Cache::Strategy S>
const Alloc::BadClusterList* NtfsVolume<S>::Impl::GetBadList() const {
  if (loading_bad) {
    return nullptr;
  }
  if (!bad_loaded) {
    bad_loaded = true;
    loading_bad = true;
    try {
      bad_list = LoadBadClusterList();
    } catch (const std::exception& e) {
      Log::Exception(e);
      bad_list.reset();
    }
    loading_bad = false;
    if (!bad_list) {
      Log::Recoverable(options.recover_errors,
                       "$BadClus is not usable: bad clusters are unknown");
    }
  }
  return bad_list.get();
}

// Loads the cluster bitmap on first use, and the bad list with it. A failure is
// cached: every later query answers unknown without trying again. When only the
// bitmap fails, a bitmap with no bits stands in, so the clusters the bad list
// names stay bad and the rest are unknown.
template <Cache::Strategy S>
Alloc::ClusterBitmap* NtfsVolume<S>::Impl::GetClusterBitmap() const {
  if (loading_bitmap) {
    return nullptr;
  }
  if (!bitmap_loaded) {
    bitmap_loaded = true;
    const Alloc::BadClusterList* bad = GetBadList();
    loading_bitmap = true;
    try {
      bitmap = LoadClusterBitmap();
    } catch (const std::exception& e) {
      Log::Exception(e);
      bitmap.reset();
    }
    loading_bitmap = false;
    if (!bitmap) {
      degraded_logged = true;
      Log::Recoverable(options.recover_errors,
                       "$Bitmap is not usable: cluster allocation is unknown");
      const ULONGLONG cluster_count = UsableClusterCount();
      if (bad != nullptr && !bad->Get().extents.empty() && cluster_count != 0) {
        bitmap = std::make_unique<Alloc::ClusterBitmap>(
            [](ULONGLONG, std::span<BYTE>) -> std::optional<ULONGLONG> {
              return std::nullopt;
            },
            0, cluster_count,
            Alloc::BitmapWindowBytes(options.tuning.alloc.bitmap_window_bytes));
      }
    }
    if (bitmap) {
      bitmap->SetBadList(bad);
      bitmap->SetRetention(KeptBitmapBytes());
    }
  }
  return bitmap.get();
}

template <Cache::Strategy S>
void NtfsVolume<S>::Impl::NoteBitmapHealth() const {
  if (bitmap && !bad_logged && bitmap->GetStatus().bad_with_clear_bit) {
    bad_logged = true;
    Log::Recoverable(options.recover_errors,
                     "Clusters listed in $BadClus are free in $Bitmap: they "
                     "are bad all the same");
  }
  if (!bitmap || degraded_logged) {
    return;
  }
  const Alloc::ClusterBitmap::Status status = bitmap->GetStatus();
  if (status.shorter_than_volume || status.failed_windows != 0) {
    degraded_logged = true;
    Log::Recoverable(options.recover_errors,
                     "$Bitmap is {}: some clusters are unknown",
                     status.shorter_than_volume ? "shorter than the volume"
                                                : "partly unreadable");
  }
}

template <Cache::Strategy S>
size_t NtfsVolume<S>::Impl::KeptBitmapBytes() const noexcept {
  return static_cast<size_t>(
      (std::min<ULONGLONG>)(bitmap_keep_bytes,
                            (std::numeric_limits<size_t>::max)()));
}

template Alloc::ClusterBitmap*
    NtfsVolume<Cache::Strategy::NoCache>::Impl::GetClusterBitmap() const;
template Alloc::ClusterBitmap*
    NtfsVolume<Cache::Strategy::FullCache>::Impl::GetClusterBitmap() const;
template const Alloc::BadClusterList*
    NtfsVolume<Cache::Strategy::NoCache>::Impl::GetBadList() const;
template const Alloc::BadClusterList*
    NtfsVolume<Cache::Strategy::FullCache>::Impl::GetBadList() const;
template void
    NtfsVolume<Cache::Strategy::NoCache>::Impl::NoteBitmapHealth() const;
template void
    NtfsVolume<Cache::Strategy::FullCache>::Impl::NoteBitmapHealth() const;
template size_t NtfsVolume<Cache::Strategy::NoCache>::Impl::KeptBitmapBytes()
    const noexcept;
template size_t NtfsVolume<Cache::Strategy::FullCache>::Impl::KeptBitmapBytes()
    const noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE
