#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The families whose inputs come from the legacy builders: hand made roots,
// records, indexes, attribute lists, names and whole volumes.
class LegacyFamilies {
 public:
  // Adds the scenarios of every family of the group, in a fixed order.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
