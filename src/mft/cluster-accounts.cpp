#include "mft/cluster-accounts.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/claim-inventory.h"
#include "alloc/cluster-account.h"
#include "alloc/cluster-sweep.h"
#include "cache/strategy.h"
#include "log/ntfs-common.h"
#include "mft/entry-table.h"
#include "mft/tree.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

template <Cache::Strategy S>
Alloc::ClusterAccountLevel
    EffectiveAccountLevel(const NtfsVolume<S>& volume,
                          const Mft::MftTree::ScanOptions& options) {
  if (!volume.GetOptions().include_deleted &&
      !volume.GetOptions().recover_errors) {
    return Alloc::ClusterAccountLevel::Off;
  }
  if (options.cluster_account == Alloc::ClusterAccountLevel::Off &&
      options.verify_data != Alloc::DataCheckMode::Off) {
    return Alloc::ClusterAccountLevel::Counts;
  }
  return options.cluster_account;
}

template <Cache::Strategy S>
void ClusterAccounts::Account(const NtfsVolume<S>& volume,
                              const Mft::MftTree::ScanOptions& options,
                              Alloc::ClaimInventory& inventory,
                              const EntryTable& table) {
  std::vector<Alloc::OwnerAccount> owners = Alloc::AccountClusters(
      volume, inventory, EffectiveAccountLevel(volume, options),
      options.verify_data);
  accounts_.reserve(owners.size());
  for (Alloc::OwnerAccount& owner : owners) {
    if (!table.IndexOf(owner.record)) {
      continue;
    }
    tally_.Add(owner.account.tally);
    accounts_.emplace_back(owner.record, std::move(owner.account));
  }
  std::ranges::sort(accounts_, {}, &decltype(accounts_)::value_type::first);
  LogSummary();
}

const Alloc::ClusterAccount* ClusterAccounts::Find(ULONGLONG record) const {
  const auto found = std::ranges::lower_bound(
      accounts_, record, {}, &decltype(accounts_)::value_type::first);
  return found != accounts_.end() && found->first == record ? &found->second
                                                            : nullptr;
}

ULONGLONG ClusterAccounts::Count() const noexcept { return accounts_.size(); }

const Alloc::ClusterTally& ClusterAccounts::Tally() const noexcept {
  return tally_;
}

void ClusterAccounts::LogSummary() const {
  Log::Info(
      "MFT scan clusters: {} files counted, {} intact, {} reallocated, {} "
      "bad, {} sparse",
      Count(), tally_.Count(Alloc::ClusterCondition::Intact),
      tally_.Count(Alloc::ClusterCondition::ReallocatedWritten) +
          tally_.Count(Alloc::ClusterCondition::ReallocatedUnwritten),
      tally_.Count(Alloc::ClusterCondition::Bad),
      tally_.Count(Alloc::ClusterCondition::Sparse));
}

template Alloc::ClusterAccountLevel
    EffectiveAccountLevel<Cache::Strategy::NoCache>(
        const NtfsVolume<Cache::Strategy::NoCache>&,
        const Mft::MftTree::ScanOptions&);
template Alloc::ClusterAccountLevel
    EffectiveAccountLevel<Cache::Strategy::FullCache>(
        const NtfsVolume<Cache::Strategy::FullCache>&,
        const Mft::MftTree::ScanOptions&);

template void ClusterAccounts::Account<Cache::Strategy::NoCache>(
    const NtfsVolume<Cache::Strategy::NoCache>&,
    const Mft::MftTree::ScanOptions&, Alloc::ClaimInventory&,
    const EntryTable&);
template void ClusterAccounts::Account<Cache::Strategy::FullCache>(
    const NtfsVolume<Cache::Strategy::FullCache>&,
    const Mft::MftTree::ScanOptions&, Alloc::ClaimInventory&,
    const EntryTable&);

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
