#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where several files claim the same clusters: freed files that
// cannot be ordered, a live file that never wrote its claim, runs that a bad
// cluster cuts, and claims over a bitmap that marks only part of them.
class ClusterRivalScenarios {
 public:
  // Adds cluster_state_rival_orders, cluster_state_merged_rivals,
  // cluster_state_bad_in_runs, and the listings of the first and the last one:
  // api_mft_list_rivals and api_mft_list_bad_in_runs.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
