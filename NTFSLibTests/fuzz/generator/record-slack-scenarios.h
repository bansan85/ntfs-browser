#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the slack of a file record holds hostile names.
class RecordSlackScenarios {
 public:
  // Adds record_slack_dense_names, record_slack_name_runs_past_record and
  // record_slack_stale_terminators.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
