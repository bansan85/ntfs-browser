#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the $Volume record is damaged.
class VolumeRecordScenarios {
 public:
  // Adds volume_information_version_below_three and
  // volume_record_corrupt_mirror_intact.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
