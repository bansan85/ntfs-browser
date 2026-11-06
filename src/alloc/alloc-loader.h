#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "mft/metadata-opener.h"
#include "volume/volume-geometry.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileReader;

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Alloc {

class BadClusterList;
class ClusterBitmap;

// A cluster bitmap with the record whose data stream it reads through.
template <Cache::Strategy S>
struct LoadedBitmap {
  // Outlives the bitmap: the bitmap reads through the data stream of this
  // record. Null when no bitmap was loaded.
  std::unique_ptr<Io::FileRecord<S>> record;
  std::unique_ptr<ClusterBitmap> bitmap;
};

// Reads the volume's allocation metadata: $Bitmap (MFT record 6) and the $Bad
// stream of $BadClus (MFT record 8). Like the volume's other metadata reads,
// none of them depends on include_deleted.
template <Cache::Strategy S>
class AllocLoader {
 public:
  // The reader, the geometry and the options MUST outlive this object.
  AllocLoader(const Mft::MetadataOpener<S>& opener,
              const Io::FileReader<S>& reader, const Volume::Geometry& geometry,
              const VolumeOptions& options) noexcept;

  // Reads $Bitmap and wraps its data stream. The bitmap is null when the
  // record or the stream is missing or unusable. With recover_errors off a
  // damaged run list rejects the attribute, so the bitmap is absent. On, the
  // runs that parse are kept, and the clusters past them read as unknown.
  [[nodiscard]] LoadedBitmap<S> LoadBitmap() const;

  // A bitmap with no bits, over cluster_count clusters: every cluster reads
  // as unknown. It stands in for a $Bitmap that cannot be read.
  [[nodiscard]] std::unique_ptr<ClusterBitmap>
      MakeUnknownBitmap(ULONGLONG cluster_count) const;

  // Reads $BadClus and copies the real runs of its $Bad stream. Null when the
  // record or the stream is missing or unusable, or when the runs are damaged
  // and recover_errors is off. The record is local: once the extents are
  // copied, its run list is released.
  [[nodiscard]] std::unique_ptr<BadClusterList> LoadBadList() const;

  // The volume's cluster count, cut at the size of the backing store when that
  // is known: a volume that claims more clusters than its store can hold is
  // cut at the store. 0 when the boot sector gave none.
  [[nodiscard]] ULONGLONG UsableClusterCount() const;

 private:
  Mft::MetadataOpener<S> opener_;
  const Io::FileReader<S>* reader_;
  const Volume::Geometry* geometry_;
  const VolumeOptions* options_;
};

}  // namespace Alloc
}  // namespace NTFS_BROWSER_NAMESPACE
