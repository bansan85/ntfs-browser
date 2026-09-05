#pragma once

#include <cstddef>

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Attr
{

// Both members are bitfields sharing the SAME 8-byte (ULONGLONG) allocation
// unit - matching src/data/index-entry.h's IndexEntry::mft_index/mft_sn
// pattern - so this struct is exactly 8 bytes, the real on-disk size of a
// $ATTRIBUTE_LIST entry's base file reference. A plain (non-bitfield)
// "WORD sequence_number;" member here instead lands AFTER the bitfield's own
// 8-byte allocation unit, inflating sizeof() to 16 (bug F11,
// docs/bug-reports/2026-09-03-full-repo.md).
struct MftSegmentReference
{
  ULONGLONG segment_number : 48;
  ULONGLONG sequence_number : 16;
};

struct AttributeList
{
  AttrType attr_type;            // Attribute type
  WORD record_size;              // Record length
  BYTE name_length;              // Name length in characters
  BYTE name_offset;              // Name offset
  ULONGLONG start_vcn;           // Start VCN
  MftSegmentReference base_ref;  // Base file reference to the attribute
  WORD attr_id;                  // Attribute Id
};

// Real on-disk size (bytes) of a nameless $ATTRIBUTE_LIST entry's fixed
// header: attr_type(4) + record_size(2) + name_length(1) + name_offset(1) +
// start_vcn(8) + base_ref(8) + attr_id(2) = 26 bytes, with no trailing
// padding on disk. Computed from the struct's own field layout (rather than
// hardcoded) so it can't silently drift out of sync again, but deliberately
// NOT sizeof(AttributeList): even with the bitfield fix above, that still
// includes trailing alignment padding (alignof(ULONGLONG) rounds it up to
// 32) that has no on-disk counterpart - see bug F11.
inline constexpr size_t kAttributeListEntryHeaderSize =
    offsetof(AttributeList, attr_id) + sizeof(AttributeList::attr_id);

}  // namespace NtfsBrowser::Attr
