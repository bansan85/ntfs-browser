#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where a run list is damaged or merges.
class RunListScenarios {
 public:
  // Adds run_list_empty_run, run_list_overflowing_run,
  // run_list_cut_after_two_runs and usn_journal_adjacent_stretches.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
