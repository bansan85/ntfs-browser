#pragma once

#include <windows.h>

// OK

namespace NtfsBrowser::Attr
{

struct MftSegmentReference
{
  ULONGLONG segment_number : 48;
  WORD sequence_number;
};

struct AttributeList
{
  DWORD attr_type;               // Attribute type
  WORD record_size;              // Record length
  BYTE name_length;              // Name length in characters
  BYTE name_offset;              // Name offset
  ULONGLONG start_vcn;           // Start VCN
  MftSegmentReference base_ref;  // Base file reference to the attribute
  WORD attr_id;                  // Attribute Id
};

}  // namespace NtfsBrowser::Attr