#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the names of a sound $MFT tree are damaged, so that the
// paths of the listings and the names the Reader decodes meet what a healthy
// volume never holds.
class PublicApiDamageScenarios {
 public:
  // Adds mft_path_chain_loop, api_mft_list_path_loop and
  // api_reader_damaged_names.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
