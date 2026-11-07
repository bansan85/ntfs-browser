#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where a scan meets the allocation bitmap, the bad cluster list
// and the $MFT bitmap.
class AllocScanScenarios {
 public:
  // Adds alloc_scan_deleted_batch, alloc_scan_live_stream,
  // alloc_bitmap_short_with_bad and the four mft_bitmap_* inputs.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
