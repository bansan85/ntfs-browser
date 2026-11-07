#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs of at most 512 bytes: boot sectors with one wrong size field, and
// four crash inputs that a fuzzer found and no structure describes.
class TinyStreamScenarios {
 public:
  // Adds boot_sectors_per_cluster_shift_out_of_range, the two
  // file_record_size_* and the two index_block_size_* boot sectors,
  // mft_addr_narrowing_error, attr_type_slot_aliasing, invalid_header_common,
  // sector_size_too_small and cluster_size_null.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
