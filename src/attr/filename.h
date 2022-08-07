#pragma once

#include <windows.h>

#include "flag/filename.h"
#include "flag/filename-namespace.h"

namespace NtfsBrowser::Attr
{

#pragma pack(1)
struct Filename
{
  ULONGLONG ParentRef;                // File reference to the parent directory
  ULONGLONG CreateTime;               // File creation time
  ULONGLONG AlterTime;                // File altered time
  ULONGLONG MFTTime;                  // MFT changed time
  ULONGLONG ReadTime;                 // File read time
  ULONGLONG AllocSize;                // Allocated size of the file
  ULONGLONG RealSize;                 // Real size of the file
  Flag::Filename flags;               // Flags
  DWORD ER;                           // Used by EAs and Reparse
  BYTE name_length;                   // Filename length in characters
  Flag::FilenameNamespace NameSpace;  // Filename space
  WORD Name[1];                       // Filename
};
#pragma pack()

}  // namespace NtfsBrowser::Attr