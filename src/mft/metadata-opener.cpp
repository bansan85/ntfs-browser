#include "mft/metadata-opener.h"

#include <ntfs-browser/win-types.h>

#include <memory>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "mft/extent-map.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

template <Cache::Strategy S>
MetadataOpener<S>::MetadataOpener(const NtfsVolume<S>& volume,
                                  const ExtentMap<S>* map) noexcept
    : volume_(&volume), map_(map) {}

template <Cache::Strategy S>
std::unique_ptr<Io::FileRecord<S>>
    MetadataOpener<S>::OpenData(ULONGLONG index) const {
  const DWORD record_size = volume_->GetFileRecordSize();
  if (map_ == nullptr ||
      !map_->IsRangeMapped(index * record_size, record_size)) {
    return {};
  }

  auto record = std::make_unique<Io::FileRecord<S>>(*volume_);
  record->SetBypassDeletedGate(true);
  record->SetAttrMask(Attr::Mask::Data);
  if (!record->ParseFileRecord(index) || !record->ParseAttrs()) {
    return {};
  }
  return record;
}

template class MetadataOpener<Cache::Strategy::NoCache>;
template class MetadataOpener<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
