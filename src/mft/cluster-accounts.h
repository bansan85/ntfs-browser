#pragma once

#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/claim-inventory.h"
#include "alloc/cluster-account.h"
#include "cache/strategy.h"
#include "mft/entry-table.h"
#include "mft/tree.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// The level of cluster accounting a scan runs at: what the options ask for, at
// least Counts when the content check is on, and Off when no entry can be
// concerned because the volume keeps neither freed nor damaged records.
template <Cache::Strategy S>
[[nodiscard]] Alloc::ClusterAccountLevel
    EffectiveAccountLevel(const NtfsVolume<S>& volume,
                          const Mft::MftTree::ScanOptions& options);

// The cluster accounts of the concerned entries of a table, ordered by record.
// Empty unless the scan accounted for clusters.
class ClusterAccounts {
 public:
  // Gives the entries of table their account, from the claims of the whole
  // scan, and logs the sums. A claimant that is no entry of table gets none.
  template <Cache::Strategy S>
  void Account(const NtfsVolume<S>& volume,
               const Mft::MftTree::ScanOptions& options,
               Alloc::ClaimInventory& inventory, const EntryTable& table);

  // The account of record, or null when it has none.
  [[nodiscard]] const Alloc::ClusterAccount* Find(ULONGLONG record) const;

  // The entries given an account.
  [[nodiscard]] ULONGLONG Count() const noexcept;

  // The sum of their tallies.
  [[nodiscard]] const Alloc::ClusterTally& Tally() const noexcept;

 private:
  void LogSummary() const;

  std::vector<std::pair<ULONGLONG, Alloc::ClusterAccount>> accounts_;
  Alloc::ClusterTally tally_;
};

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
