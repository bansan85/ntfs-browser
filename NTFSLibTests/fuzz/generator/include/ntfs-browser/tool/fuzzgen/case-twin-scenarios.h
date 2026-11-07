#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the index holds names that are equal when upcased.
class CaseTwinScenarios {
 public:
  // Adds case_twin_root, case_twin_blocks_6, case_twin_blocks_13,
  // case_twin_stale_exact, case_twin_unordered and case_twin_hard_links.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
