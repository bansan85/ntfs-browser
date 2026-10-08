#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>

namespace NtfsBrowser::Data {

////////////////////////////////
// List to hold parsed DataRuns
////////////////////////////////
struct RunEntry {
  std::optional<ULONGLONG> lcn;  // empty to indicate sparse data
  ULONGLONG clusters = 0;
  ULONGLONG start_vcn = 0;
  ULONGLONG last_vcn = 0;
};

// typedef class CSList<DataRun_Entry> CDataRunList;

}  // namespace NtfsBrowser::Data
