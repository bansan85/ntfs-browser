#pragma once

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "alloc/claim-inventory.h"
#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Alloc {

// The account of one concerned record.
struct OwnerAccount {
  ULONGLONG record{0};
  ClusterAccount account;
};

// Works out the account of every concerned record of inventory, in one pass in
// LCN order over its claims, the cluster bitmap and the bad cluster list: the
// claims are sorted, cut into stretches where the set of claimants and the
// bitmap are constant, and each concerned claimant gets the first condition
// that fits (see ClusterCondition). It reads the bitmap once, in ascending
// order, through NtfsVolume::GetClusterRuns(), and the data only for the
// content check of check. The claims of inventory are sorted by the call.
// Records without a cluster to count get no account.
template <Cache::Strategy S>
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::vector<OwnerAccount>
    AccountClusters(const NtfsVolume<S>& volume, ClaimInventory& inventory,
                    ClusterAccountLevel level, DataCheckMode check);

}  // namespace Alloc

}  // namespace NTFS_BROWSER_NAMESPACE
