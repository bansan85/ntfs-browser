#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where a name is carved twice or from bytes that are not a name.
class SlackNamesScenarios {
 public:
  // Adds record_slack_name_units, record_slack_rejected_fields,
  // record_slack_repeated_names, record_slack_live_names and
  // index_slack_repeated_names.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
