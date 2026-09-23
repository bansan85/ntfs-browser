#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>

namespace NtfsBrowser::Attr
{

// Both members are bitfields sharing one ULONGLONG allocation unit, so
// this struct is exactly 8 bytes, the real on-disk base file reference size.
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

// Real on-disk size of a nameless $ATTRIBUTE_LIST entry's fixed header.
inline constexpr size_t kAttributeListEntryHeaderSize =
    offsetof(AttributeList, attr_id) + sizeof(AttributeList::attr_id);

}  // namespace NtfsBrowser::Attr
