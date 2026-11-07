#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/case-twin-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/mft-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/run-list-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/scenario-catalog.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

ScenarioRegistry ScenarioCatalog::Build() {
  ScenarioRegistry registry;
  CaseTwinScenarios::Register(registry);
  MftScanScenarios::Register(registry);
  RunListScenarios::Register(registry);
  return registry;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
