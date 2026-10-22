#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "data/run-entry.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

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

  // The real (non-sparse) clusters the list maps, and how far it reaches.
  struct ExtentList {
    std::vector<Alloc::Extent> extents;
    // Clusters from the first VCN through the end of the last non-empty run,
    // sparse runs included.
    ULONGLONG covered_clusters{0};
    // False when a run has zero length, or when lcn + clusters overflows 64
    // bits. Such a run is damaged: an overflowing one is still listed, so the
    // caller sees it as out of range, a zero-length one is left out.
    bool well_formed{true};
  };

  // The extents of every real run, in VCN order. Sparse runs own no cluster.
  [[nodiscard]] ExtentList Extents() const;

  // One stretch of real clusters, at a VCN. Runs that touch are one stretch.
  struct VcnRun {
    ULONGLONG start_vcn{0};
    ULONGLONG clusters{0};
  };

  // The real stretches the list maps, with where they sit in the stream.
  struct VcnRunList {
    std::vector<VcnRun> runs;
    // Same meaning as in ExtentList.
    ULONGLONG covered_clusters{0};
    // False when a run has zero length, or when lcn + clusters overflows 64
    // bits. Unlike Extents(), an overflowing run is left out.
    bool well_formed{true};
  };

  // The real stretches of the list in VCN order, sparse runs left out and
  // touching real runs merged.
  [[nodiscard]] VcnRunList RealRanges() const;

  // The run holding vcn, or nullptr if no run holds it.
  [[nodiscard]] const Data::RunEntry* Find(ULONGLONG vcn) const noexcept;

  // Real clusters at the start of the compression unit at unit_first_vcn.
  // An empty optional if the unit is not fully mapped, or if a real run
  // follows a hole within the unit.
  [[nodiscard]] std::optional<ULONGLONG>
      LeadingRealClusters(ULONGLONG unit_first_vcn,
                          ULONGLONG unit_clusters) const noexcept;

 private:
  // Where one Parse() pass stands: the running LCN and VCN, and the VCN span
  // every run must stay within.
  struct DecodeCursor {
    LONGLONG lcn = 0;
    ULONGLONG vcn = 0;
    ULONGLONG vcn_bound = 0;
  };

  std::vector<Data::RunEntry> runs_;

  // VCN count this list covers. Starts at the header's span and grows on
  // Append(), which is why it can exceed the runs' own last VCN.
  ULONGLONG clusters_;

  [[nodiscard]] static bool PickData(std::span<const BYTE>& data_run,
                                     ULONGLONG& length,
                                     std::optional<LONGLONG>& lcn_offset,
                                     bool recover) noexcept;
  [[nodiscard]] bool AppendDataRun(ULONGLONG length,
                                   std::optional<LONGLONG> lcn_offset,
                                   DecodeCursor& cursor, bool recover);
};  // RunList

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
