#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs of the first fake images where the root directory record holds
// nothing but an $ATTRIBUTE_LIST of one 26-byte entry, and one field of that
// entry, or the record it names, is damaged.
class LegacyAttributeListScenarios {
 public:
  // Adds the inputs of the family.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
