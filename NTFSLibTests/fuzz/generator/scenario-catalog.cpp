#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/alloc-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/attribute-list-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/bad-cluster-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/boot-sector-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/case-twin-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/cluster-bitmap-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/cluster-state-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/mft-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/run-list-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/scenario-catalog.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/tiny-stream-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/usn-journal-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/usn-tree-scenarios.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

ScenarioRegistry ScenarioCatalog::Build() {
  ScenarioRegistry registry;
  AllocScanScenarios::Register(registry);
  BadClusterScenarios::Register(registry);
  ClusterStateScenarios::Register(registry);
  UsnJournalScenarios::Register(registry);
  UsnTreeScenarios::Register(registry);
  BootSectorScenarios::Register(registry);
  CaseTwinScenarios::Register(registry);
  ClusterBitmapScenarios::Register(registry);
  TinyStreamScenarios::Register(registry);
  MftScanScenarios::Register(registry);
  RunListScenarios::Register(registry);
  AttributeListScanScenarios::Register(registry);
  return registry;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
