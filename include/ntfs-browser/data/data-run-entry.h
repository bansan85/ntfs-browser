#pragma once

#include <windows.h>

namespace NtfsBrowser
{
////////////////////////////////
// List to hold parsed DataRuns
////////////////////////////////
struct DataRunEntry
{
  LONGLONG LCN;  // -1 to indicate sparse data
  ULONGLONG Clusters;
  ULONGLONG StartVCN;
  ULONGLONG LastVCN;
};
//typedef class CSList<DataRun_Entry> CDataRunList;

}  // namespace NtfsBrowser