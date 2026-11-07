#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the $MFT, its mirror or its data runs are damaged.
class MftFixtureScenarios {
 public:
  // Adds mft_data_extent_chain, mft_data_without_base_extent,
  // mft_mirror_same_as_mft, mft_record0_corrupt_mirror_intact,
  // mft_tree_full_cache, mft_tree_huge_real_size and
  // mft_tree_names_damaged_record_and_duplicate_link.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
