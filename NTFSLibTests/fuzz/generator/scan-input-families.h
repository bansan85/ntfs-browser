#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The families whose inputs feed a scan of the $MFT, its run lists, its
// attribute lists and the slack of its records and indexes.
class ScanInputFamilies {
 public:
  // Adds the scenarios of every family of the group, in a fixed order.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
