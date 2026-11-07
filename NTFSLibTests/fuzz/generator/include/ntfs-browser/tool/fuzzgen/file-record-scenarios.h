#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where a file record, or an index block it points to, is damaged.
class FileRecordScenarios {
 public:
  // Adds filerecord_filename_bad_lengths,
  // filerecord_walk_runs_past_record_end, filerecord_index_alloc_missing and
  // filerecord_index_subnode_self_loop.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
