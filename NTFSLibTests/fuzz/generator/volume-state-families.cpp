#include "volume-state-families.h"

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "alloc-scan-scenarios.h"
#include "bad-cluster-scenarios.h"
#include "cluster-state-scenarios.h"
#include "usn-journal-scenarios.h"
#include "usn-tree-scenarios.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void VolumeStateFamilies::Register(ScenarioRegistry& registry) {
  AllocScanScenarios::Register(registry);
  BadClusterScenarios::Register(registry);
  ClusterStateScenarios::Register(registry);
  UsnJournalScenarios::Register(registry);
  UsnTreeScenarios::Register(registry);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
