#include "io/raw-attr-headers.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/header-common.h"
#include "attr/run-list.h"
#include "attr/type.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

RawAttrHeaders::RawAttrHeaders(const Data::FileRecordHeader& record,
                               DWORD record_size) noexcept
    : record_(record), record_size_(record_size) {}

std::optional<DWORD> RawAttrHeaders::Find(Attr::Type attr_type) const noexcept {
  DWORD data_ptr = record_.offset_of_attr;
  // Each header is read through a copy: its position comes from the disk.
  while (static_cast<ULONGLONG>(data_ptr) + sizeof(Attr::HeaderCommon) <=
         record_size_) {
    Attr::HeaderCommon head{};
    std::memcpy(&head, &record_.raw[data_ptr], sizeof(head));
    if (head.type == Attr::Type::All || head.total_size == 0 ||
        static_cast<ULONGLONG>(data_ptr) + head.total_size > record_size_) {
      return std::nullopt;
    }
    if (head.type == attr_type) {
      return data_ptr;
    }
    data_ptr += head.total_size;
  }
  return std::nullopt;
}

std::optional<Data::HeaderNonResident>
    RawAttrHeaders::ReadListHeader(DWORD at) const {
  if (record_size_ - at < sizeof(Data::HeaderNonResident)) {
    return std::nullopt;
  }
  Data::HeaderNonResident header{};
  std::memcpy(&header, &record_.raw[at], sizeof(header));
  if (header.header.non_resident == 0 ||
      header.header.total_size < sizeof(header) ||
      Data::HasCompressedSizeField(header) || header.start_vcn != 0 ||
      header.data_run_offset >= header.header.total_size) {
    return std::nullopt;
  }
  return header;
}

bool RawAttrHeaders::Has(Attr::Type attr_type) const noexcept {
  return Find(attr_type).has_value();
}

std::vector<Alloc::Extent>
    RawAttrHeaders::AttributeListExtents(ULONGLONG cluster_size,
                                         ULONGLONG max_bytes) const {
  if (max_bytes == 0 || cluster_size == 0) {
    return {};
  }
  const std::optional<DWORD> at = Find(Attr::Type::AttributeList);
  if (!at) {
    return {};
  }
  const std::optional<Data::HeaderNonResident> header = ReadListHeader(*at);
  if (!header) {
    return {};
  }

  // Find() checked that the attribute lies in the record.
  const std::span<const BYTE> run_bytes =
      std::span<const BYTE>{&record_.raw[*at], header->header.total_size}
          .subspan(header->data_run_offset);
  Attr::RunList runs(Data::SpannedClusters(*header));
  // Recovering: a damaged run list gives the runs before the damage, and is
  // logged as recovered, not as an error. ParseAttrs() judges it.
  runs.Parse(run_bytes, Data::VcnSpan(*header), true);

  const ULONGLONG size = (std::min)(header->real_size, max_bytes);
  const ULONGLONG end_vcn =
      size / cluster_size + (size % cluster_size != 0 ? 1 : 0);
  // The byte address of a cluster MUST fit a LONGLONG, as in ReadClusters().
  const ULONGLONG max_lcn =
      static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)()) /
      cluster_size;
  return runs.ExtentsBetween(0, end_vcn, max_lcn);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
