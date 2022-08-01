#pragma once

#include <windows.h>

namespace NtfsBrowser::Data
{

struct AttrFilename
{
  ULONGLONG ParentRef;   // File reference to the parent directory
  ULONGLONG CreateTime;  // File creation time
  ULONGLONG AlterTime;   // File altered time
  ULONGLONG MFTTime;     // MFT changed time
  ULONGLONG ReadTime;    // File read time
  ULONGLONG AllocSize;   // Allocated size of the file
  ULONGLONG RealSize;    // Real size of the file
  DWORD Flags;           // Flags
  DWORD ER;              // Used by EAs and Reparse
  BYTE NameLength;       // Filename length in characters
  BYTE NameSpace;        // Filename space
  WORD Name[1];          // Filename
};
}  // namespace NtfsBrowser::Data