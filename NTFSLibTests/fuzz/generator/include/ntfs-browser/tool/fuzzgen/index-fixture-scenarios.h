#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the index allocation is split or holds many blocks.
class IndexFixtureScenarios {
 public:
  // Adds index_allocation_split_runs and index_orphan_scan_huge_block_count.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
