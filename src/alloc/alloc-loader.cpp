#include "alloc/alloc-loader.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "alloc/bad-cluster-list.h"
#include "alloc/cluster-bitmap.h"
#include "attr/base.h"
#include "bench/bench.h"
#include "bench/recorder.h"
#include "cache/strategy.h"
#include "io/file-reader.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/idx.h"
#include "mft/metadata-opener.h"
#include "volume-options.h"
#include "volume/volume-geometry.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

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

template <Cache::Strategy S>
AllocLoader<S>::AllocLoader(const Mft::MetadataOpener<S>& opener,
                            const Io::FileReader<S>& reader,
                            const Volume::Geometry& geometry,
                            const VolumeOptions& options) noexcept
    : opener_(opener),
      reader_(&reader),
      geometry_(&geometry),
      options_(&options) {}

template <Cache::Strategy S>
LoadedBitmap<S> AllocLoader<S>::LoadBitmap() const {
  auto record = opener_.OpenData(static_cast<ULONGLONG>(Mft::Idx::Bitmap));
  if (!record) {
    return {};
  }

  const Attr::AttrBase<S>* data = record->FindStream({});
  if (data == nullptr) {
    return {};
  }

  // The store cannot hold more clusters than its size says, and a volume that
  // claims more is cut at the store. Bytes of bitmap past what those clusters
  // need are never looked at, so a forged size cannot make a query long.
  ULONGLONG cluster_count = geometry_->cluster_count;
  ULONGLONG data_size = data->GetDataSize();
  if (const std::optional<ULONGLONG> store_bytes = reader_->Size();
      store_bytes && geometry_->cluster_size != 0) {
    const ULONGLONG store_clusters = *store_bytes / geometry_->cluster_size;
    cluster_count =
        cluster_count == 0 ? 0 : std::min(cluster_count, store_clusters);
    data_size = std::min(data_size, store_clusters / 8 + 1);
  }
  if (cluster_count != 0) {
    data_size = std::min(data_size, cluster_count / 8 + 1);
  }

  const DWORD piece = geometry_->cluster_size;
  auto read_stream = [data, piece](ULONGLONG offset, std::span<BYTE> out) {
    return ReadBitmapBytes<S>(*data, piece, offset, out);
  };
  auto bitmap = std::make_unique<ClusterBitmap>(
      read_stream, data_size, cluster_count,
      BitmapWindowBytes(options_->tuning.alloc.bitmap_window_bytes));
  return {.record = std::move(record), .bitmap = std::move(bitmap)};
}

template <Cache::Strategy S>
std::unique_ptr<ClusterBitmap>
    AllocLoader<S>::MakeUnknownBitmap(ULONGLONG cluster_count) const {
  return std::make_unique<ClusterBitmap>(
      [](ULONGLONG, std::span<BYTE>) -> std::optional<ULONGLONG> {
        return std::nullopt;
      },
      0, cluster_count,
      BitmapWindowBytes(options_->tuning.alloc.bitmap_window_bytes));
}

template <Cache::Strategy S>
std::unique_ptr<BadClusterList> AllocLoader<S>::LoadBadList() const {
  const auto record =
      opener_.OpenData(static_cast<ULONGLONG>(Mft::Idx::BadCluster));
  if (!record) {
    return {};
  }

  const Attr::AttrBase<S>* stream = record->FindStream(L"$Bad");
  if (stream == nullptr) {
    return {};
  }
  return BadClusterList::Build(stream->GetExtents(), UsableClusterCount(),
                               options_->recover_errors);
}

template <Cache::Strategy S>
ULONGLONG AllocLoader<S>::UsableClusterCount() const {
  ULONGLONG cluster_count = geometry_->cluster_count;
  if (const std::optional<ULONGLONG> store_bytes = reader_->Size();
      store_bytes && geometry_->cluster_size != 0 && cluster_count != 0) {
    cluster_count =
        std::min(cluster_count, *store_bytes / geometry_->cluster_size);
  }
  return cluster_count;
}

template class AllocLoader<Cache::Strategy::NoCache>;
template class AllocLoader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
