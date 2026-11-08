#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where one field of the boot sector is wrong, on the start of the
// base volume, and the dirty volume.
class BootSectorScenarios {
 public:
  // Adds the ten boot_sector_* inputs with a damaged field,
  // boot_sector_backup_fallback and volume_dirty_flag_set.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
