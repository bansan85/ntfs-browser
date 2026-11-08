#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs of the first fake images where the root directory holds its own
// $INDEX_ROOT with one entry, and one field of the root record, of its index
// or of the $MFT record is damaged.
class LegacyIndexRootScenarios {
 public:
  // Adds the inputs of the family.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
