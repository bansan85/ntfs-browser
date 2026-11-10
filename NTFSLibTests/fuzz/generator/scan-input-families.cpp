#include "scan-input-families.h"

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "attribute-list-scan-scenarios.h"
#include "boot-sector-scenarios.h"
#include "case-twin-scenarios.h"
#include "cluster-bitmap-scenarios.h"
#include "index-slack-scenarios.h"
#include "mft-huge-scan-scenarios.h"
#include "mft-scan-scenarios.h"
#include "public-api-scenarios.h"
#include "record-slack-scenarios.h"
#include "run-list-scenarios.h"
#include "slack-names-scenarios.h"
#include "tiny-stream-scenarios.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void ScanInputFamilies::Register(ScenarioRegistry& registry) {
  BootSectorScenarios::Register(registry);
  CaseTwinScenarios::Register(registry);
  ClusterBitmapScenarios::Register(registry);
  TinyStreamScenarios::Register(registry);
  MftScanScenarios::Register(registry);
  RunListScenarios::Register(registry);
  AttributeListScanScenarios::Register(registry);
  IndexSlackScenarios::Register(registry);
  RecordSlackScenarios::Register(registry);
  SlackNamesScenarios::Register(registry);
  MftHugeScanScenarios::Register(registry);
  PublicApiScenarios::Register(registry);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
