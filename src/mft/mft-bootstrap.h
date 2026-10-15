#pragma once

#include <ntfs-browser/win-types.h>

#include <list>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

#include "mft/extent-map.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// Reads $MFT's own record and maps the DATA extents of $MFT, which every other
// file record read goes through. Owns that record, and the extension records
// that carry $MFT's continuations.
template <Cache::Strategy S>
class MftBootstrap {
 public:
  // The volume the records are built over. It MUST outlive this object.
  explicit MftBootstrap(NtfsVolume<S>& volume);

  // Parses $MFT's base record for its DATA attribute. False when it does not
  // parse.
  [[nodiscard]] bool ParseRecord();

  // Finds $MFT's base DATA extent: the unnamed, non-resident instance at VCN 0.
  // Null when there is none.
  [[nodiscard]] const Attr::AttrBase<S>* FindBaseExtent() const;

  // Builds the map from base_extent, then accepts the continuations its
  // $ATTRIBUTE_LIST names. The cluster geometry comes from the boot sector.
  void MapExtents(const Attr::AttrBase<S>& base_extent, DWORD cluster_size,
                  DWORD file_record_size);

  // The map MapExtents() built. Empty until then.
  [[nodiscard]] const ExtentMap<S>& Map() const noexcept;

 private:
  // One extension record $MFT's $ATTRIBUTE_LIST names for DATA, with the
  // sequence number its entries claim and the start VCN of each entry.
  struct PendingMftExtension {
    ULONGLONG record{0};
    WORD sequence{0};
    std::vector<ULONGLONG> start_vcns;
  };

  void ResolveMftDataExtents();
  [[nodiscard]] static std::vector<PendingMftExtension>
      CollectPendingMftExtensions(const Attr::AttrBase<S>& raw_list,
                                  ULONGLONG self_ref);
  void ResolvePendingMftExtension(const PendingMftExtension& item,
                                  ULONGLONG self_ref);
  void WarnIfClaimsMoreThanMapped(const Attr::AttrBase<S>& base_extent) const;

  // A pointer, not a reference, as with the volume's own Impl::self.
  NtfsVolume<S>* volume_;

  // The map points into the attributes of the records below, so it is declared
  // first and destroyed last.
  ExtentMap<S> map_{0, 0};
  Io::FileRecord<S> record_;
  // Owns extension FileRecords; std::list keeps FullCache pointers stable.
  std::list<Io::FileRecord<S>> extension_records_;

  DWORD file_record_size_{0};
};

}  // namespace Mft

}  // namespace NTFS_BROWSER_NAMESPACE
