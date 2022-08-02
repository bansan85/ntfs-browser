#pragma once

#include <windows.h>

namespace NtfsBrowser::Attr
{

#pragma pack(1)
struct AttributeList
{
  DWORD AttrType;      // Attribute type
  WORD RecordSize;     // Record length
  BYTE NameLength;     // Name length in characters
  BYTE NameOffset;     // Name offset
  ULONGLONG StartVCN;  // Start VCN
  ULONGLONG BaseRef;   // Base file reference to the attribute
  WORD AttrId;         // Attribute Id
};
#pragma pack()

}  // namespace NtfsBrowser::Attr