#pragma once

#include <windows.h>

namespace NtfsBrowser
{

// Attribute: ATTRIBUTE_LIST
struct AttrAttributeList
{
  DWORD AttrType;      // Attribute type
  WORD RecordSize;     // Record length
  BYTE NameLength;     // Name length in characters
  BYTE NameOffset;     // Name offset
  ULONGLONG StartVCN;  // Start VCN
  ULONGLONG BaseRef;   // Base file reference to the attribute
  WORD AttrId;         // Attribute Id
};

}  // namespace NtfsBrowser