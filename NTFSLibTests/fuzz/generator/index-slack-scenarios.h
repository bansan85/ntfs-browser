#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the slack of an index block holds hostile names.
class IndexSlackScenarios {
 public:
  // Adds index_slack_dense_names, index_slack_name_runs_past_block and
  // index_slack_damaged_blocks.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
