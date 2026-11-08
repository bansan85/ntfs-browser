#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the $Bitmap file (cluster allocation) is odd in shape or
// size, and the one where an index $BITMAP attribute spans several clusters.
class ClusterBitmapScenarios {
 public:
  // Adds the five alloc_bitmap_* inputs and bitmap_multi_cluster.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
