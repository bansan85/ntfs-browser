#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs of the volume that the first author wrote by hand: a base volume
// // whose $MFT holds 16 clusters of file records in the layout of a real NTFS
// // volume, and one or two records of a file with a damaged stream.
class LegacyVolumeScenarios {
 public:
  // Adds the inputs of the family.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
