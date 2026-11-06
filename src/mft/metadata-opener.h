#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

template <Cache::Strategy S>
class NtfsVolume;

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Mft {

template <Cache::Strategy S>
class ExtentMap;

// Opens the records of the volume's metadata files ($UpCase, $Bitmap,
// $BadClus) for their $DATA attributes. A cheap handle: it copies nothing and
// reads nothing until OpenData().
template <Cache::Strategy S>
class MetadataOpener {
 public:
  // The volume MUST outlive this object. A null map stands for a volume that
  // is not open: no record opens then. The map MUST outlive this object too.
  MetadataOpener(const NtfsVolume<S>& volume, const ExtentMap<S>* map) noexcept;

  // Parses MFT record index with its $DATA attributes only. Null when $MFT
  // does not map the record, or the record does not parse. Like the volume's
  // other metadata reads, it MUST NOT depend on include_deleted.
  [[nodiscard]] std::unique_ptr<Io::FileRecord<S>>
      OpenData(ULONGLONG index) const;

 private:
  const NtfsVolume<S>* volume_;
  const ExtentMap<S>* map_;
};

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
