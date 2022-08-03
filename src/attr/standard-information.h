#pragma once

#include <windows.h>

#include "../flag/std-info-permission.h"

namespace NtfsBrowser::Attr
{

#pragma pack(1)
struct StandardInformation
{
  ULONGLONG CreateTime;                // File creation time
  ULONGLONG AlterTime;                 // File altered time
  ULONGLONG MFTTime;                   // MFT changed time
  ULONGLONG ReadTime;                  // File read time
  Flag::StdInfoPermission Permission;  // Dos file permission
  DWORD MaxVersionNo;                  // Maxim number of file versions
  DWORD VersionNo;                     // File version number
  DWORD ClassId;                       // Class Id
  DWORD OwnerId;                       // Owner Id
  DWORD SecurityId;                    // Security Id
  ULONGLONG QuotaCharged;              // Quota charged
  ULONGLONG USN;                       // USN Journel
};
#pragma pack()

}  // namespace NtfsBrowser::Attr