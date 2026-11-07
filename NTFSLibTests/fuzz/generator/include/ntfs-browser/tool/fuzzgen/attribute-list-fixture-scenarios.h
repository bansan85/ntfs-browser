#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where an $ATTRIBUTE_LIST is split or cut.
class AttributeListFixtureScenarios {
 public:
  // Adds attribute_list_split_attribute and attr_list_nonres_entry_past_end.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
