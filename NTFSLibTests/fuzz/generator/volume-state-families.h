#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The families whose inputs rest on the allocation state of a volume: the
// bitmaps, the bad cluster list and the USN journal.
class VolumeStateFamilies {
 public:
  // Adds the scenarios of every family of the group, in a fixed order.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
