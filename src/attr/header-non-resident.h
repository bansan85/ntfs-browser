#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser::Attr
{
struct HeaderNonResident
{
  AttrHeaderCommon header;  // Common data structure
  ULONGLONG start_vcn;      // Starting VCN
  ULONGLONG last_vcn;       // Last VCN
  WORD data_run_offset;     // Offset to the Data Runs
  WORD comp_unit_size;      // Compression unit size
  DWORD padding;            // Padding
  ULONGLONG alloc_size;     // Allocated size of the attribute
  ULONGLONG real_size;      // Real size of the attribute
  ULONGLONG ini_size;       // Initialized data size of the stream
};
}  // namespace NtfsBrowser::Attr
