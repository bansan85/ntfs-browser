#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser
{
struct AttrHeaderNonResident
{
  AttrHeaderCommon Header;  // Common data structure
  ULONGLONG StartVCN;       // Starting VCN
  ULONGLONG LastVCN;        // Last VCN
  WORD DataRunOffset;       // Offset to the Data Runs
  WORD CompUnitSize;        // Compression unit size
  DWORD Padding;            // Padding
  ULONGLONG AllocSize;      // Allocated size of the attribute
  ULONGLONG RealSize;       // Real size of the attribute
  ULONGLONG IniSize;        // Initialized data size of the stream
};
}  // namespace NtfsBrowser
