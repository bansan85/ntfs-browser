#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "data/run-entry.h"

namespace NtfsBrowser::Attr {

////////////////////////////////
// Data runs of a non-resident attribute
////////////////////////////////
// Decoded runs of one non-resident attribute, mapped to VCN ranges. Does not
// depend on the cache strategy or on the volume.
class RunList {
 public:
  // Starts with no runs. clusters is the VCN count this list already covers.
  explicit RunList(ULONGLONG clusters) noexcept;

  // Decodes run_bytes into runs, appended to the list. vcn_bound is the
  // declared VCN span (last_vcn - start_vcn). When recover is false, a decode
  // error throws; when true, decoding stops and the runs so far are kept.
  void Parse(std::span<const BYTE> run_bytes, ULONGLONG vcn_bound,
             bool recover);

  // Splices other's runs onto this list, as the next VCN range.
  void Append(const RunList& other);

  // VCN count this list covers, including appended lists.
  [[nodiscard]] ULONGLONG TotalClusters() const noexcept;

  // Clusters from the first VCN through the last run that maps real clusters.
  [[nodiscard]] ULONGLONG MappedClusters() const noexcept;

  // The run holding vcn, or nullptr if no run holds it.
  [[nodiscard]] const Data::RunEntry* Find(ULONGLONG vcn) const noexcept;

  // Real clusters at the start of the compression unit at unit_first_vcn.
  // An empty optional if the unit is not fully mapped, or if a real run
  // follows a hole within the unit.
  [[nodiscard]] std::optional<ULONGLONG>
      LeadingRealClusters(ULONGLONG unit_first_vcn,
                          ULONGLONG unit_clusters) const noexcept;

 private:
  std::vector<Data::RunEntry> runs_;

  // VCN count this list covers. Starts at the header's span and grows on
  // Append(), which is why it can exceed the runs' own last VCN.
  ULONGLONG clusters_;

  [[nodiscard]] static bool PickData(std::span<const BYTE>& data_run,
                                     ULONGLONG& length, LONGLONG& lcn_offset,
                                     bool recover) noexcept;
  [[nodiscard]] bool AppendDataRun(ULONGLONG length, LONGLONG lcn_offset,
                                   LONGLONG& lcn, ULONGLONG& vcn,
                                   ULONGLONG vcn_bound, bool recover);
};  // RunList

}  // namespace NtfsBrowser::Attr
