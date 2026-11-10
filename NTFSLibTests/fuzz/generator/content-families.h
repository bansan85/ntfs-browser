#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The families whose inputs depend on file content: compressed files, EFS
// files, data run clusters and the corpus traces.
class ContentFamilies {
 public:
  // Adds the scenarios of every family of the group, in a fixed order.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
