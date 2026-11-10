#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The families whose inputs are small fixtures of one record, one volume
// record, one attribute list or one index.
class FixtureFamilies {
 public:
  // Adds the scenarios of every family of the group, in a fixed order.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
