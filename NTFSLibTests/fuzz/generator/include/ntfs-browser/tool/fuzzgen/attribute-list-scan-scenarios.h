#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where an $ATTRIBUTE_LIST spreads a file over records or clusters.
class AttributeListScanScenarios {
 public:
  // Adds attribute_list_scan_both_sides, attribute_list_scan_foreign_extension,
  // attribute_list_multi_cluster, attribute_list_scan_non_resident,
  // attribute_list_overwritten_invalid_type,
  // attribute_list_overwritten_cut_entry, scan_threads_waiting_strict and
  // scan_threads_waiting_recovering.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
