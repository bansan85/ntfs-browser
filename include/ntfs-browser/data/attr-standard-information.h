#pragma once

#include <windows.h>

namespace NtfsBrowser
{

// Attribute: STANDARD_INFORMATION
struct AttrStandardInformation
{
  ULONGLONG CreateTime;    // File creation time
  ULONGLONG AlterTime;     // File altered time
  ULONGLONG MFTTime;       // MFT changed time
  ULONGLONG ReadTime;      // File read time
  DWORD Permission;        // Dos file permission
  DWORD MaxVersionNo;      // Maxim number of file versions
  DWORD VersionNo;         // File version number
  DWORD ClassId;           // Class Id
  DWORD OwnerId;           // Owner Id
  DWORD SecurityId;        // Security Id
  ULONGLONG QuotaCharged;  // Quota charged
  ULONGLONG USN;           // USN Journel
};

}  // namespace NtfsBrowser