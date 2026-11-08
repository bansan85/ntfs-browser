#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where a change journal meets the file tree, and the stream that
// the journal scanner meets as a file of its own.
class UsnTreeScenarios {
 public:
  // Adds usn_tree_annotated, usn_tree_journal_only_paths, the four
  // usn_loader_* inputs and usn_scanner_damaged_names.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
