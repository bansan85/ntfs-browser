#pragma once

#include <windows.h>

#include "../flag/std-info-permission.h"

namespace NtfsBrowser::Attr
{

struct StandardInformation
{
  ULONGLONG create_time;               // File creation time
  ULONGLONG alter_time;                // File altered time
  ULONGLONG mft_time;                  // MFT changed time
  ULONGLONG read_time;                 // File read time
  Flag::StdInfoPermission permission;  // Dos file permission
  DWORD max_version_no;                // Maxim number of file versions
  DWORD version_no;                    // File version number
  DWORD class_id;                      // Class Id
  DWORD owner_id;                      // Owner Id
  DWORD security_id;                   // Security Id
  ULONGLONG quota_charged;             // Quota charged
  ULONGLONG usn;                       // USN Journel
};

}  // namespace NtfsBrowser::Attr