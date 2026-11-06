#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"
#include "data/header-non-resident.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// Reads the attribute headers of one file record from its bytes alone. Nothing
// is parsed into an attribute, and nothing is read from the disk: it answers
// questions that a caller can settle before it pays for a full parse.
class RawAttrHeaders {
 public:
  // record MUST outlive this object. record_size is the size of the record in
  // bytes, as the volume reports it: the headers are never read past it.
  RawAttrHeaders(const Data::FileRecordHeader& record,
                 DWORD record_size) noexcept;

  // Whether the record has an attribute header of type attr_type, going by the
  // headers alone. False when a header before it is broken.
  [[nodiscard]] bool Has(Attr::Type attr_type) const noexcept;

  // The disk extents that hold the first max_bytes bytes of a non-resident
  // $ATTRIBUTE_LIST, with clusters of cluster_size bytes. Empty for a resident
  // list, a compressed one, one that does not map its bytes from VCN 0, or a
  // record without a list. A damaged run list gives the runs before the
  // damage.
  [[nodiscard]] std::vector<Alloc::Extent>
      AttributeListExtents(ULONGLONG cluster_size, ULONGLONG max_bytes) const;

 private:
  // Offset in the record of its first attribute header of type attr_type.
  // Empty when there is none, or when a header before it is broken.
  [[nodiscard]] std::optional<DWORD> Find(Attr::Type attr_type) const noexcept;

  // The non-resident header of the attribute at offset at. Empty unless it is
  // a whole, uncompressed list that maps its bytes from VCN 0.
  [[nodiscard]] std::optional<Data::HeaderNonResident>
      ReadListHeader(DWORD at) const;

  const Data::FileRecordHeader& record_;
  DWORD record_size_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
