#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-catalog.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "alloc-scan-scenarios.h"
#include "attribute-list-fixture-scenarios.h"
#include "attribute-list-scan-scenarios.h"
#include "bad-cluster-scenarios.h"
#include "boot-sector-scenarios.h"
#include "case-twin-scenarios.h"
#include "cluster-bitmap-scenarios.h"
#include "cluster-state-scenarios.h"
#include "compressed-file-scenarios.h"
#include "corpus-trace-scenarios.h"
#include "data-run-lcn-scenarios.h"
#include "efs-fixture-scenarios.h"
#include "file-record-scenarios.h"
#include "index-fixture-scenarios.h"
#include "index-slack-scenarios.h"
#include "legacy-attribute-list-scenarios.h"
#include "legacy-builder-scenarios.h"
#include "legacy-index-block-scenarios.h"
#include "legacy-index-root-scenarios.h"
#include "legacy-name-scenarios.h"
#include "legacy-residency-scenarios.h"
#include "legacy-root-record-scenarios.h"
#include "legacy-volume-scenarios.h"
#include "mft-fixture-scenarios.h"
#include "mft-huge-scan-scenarios.h"
#include "mft-scan-scenarios.h"
#include "record-slack-scenarios.h"
#include "run-list-scenarios.h"
#include "slack-names-scenarios.h"
#include "tiny-stream-scenarios.h"
#include "usn-journal-scenarios.h"
#include "usn-tree-scenarios.h"
#include "volume-record-scenarios.h"

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
  IndexSlackScenarios::Register(registry);
  RecordSlackScenarios::Register(registry);
  SlackNamesScenarios::Register(registry);
  MftFixtureScenarios::Register(registry);
  FileRecordScenarios::Register(registry);
  VolumeRecordScenarios::Register(registry);
  AttributeListFixtureScenarios::Register(registry);
  IndexFixtureScenarios::Register(registry);
  LegacyIndexRootScenarios::Register(registry);
  LegacyRootRecordScenarios::Register(registry);
  LegacyBuilderScenarios::Register(registry);
  LegacyAttributeListScenarios::Register(registry);
  LegacyIndexBlockScenarios::Register(registry);
  LegacyResidencyScenarios::Register(registry);
  MftHugeScanScenarios::Register(registry);
  LegacyVolumeScenarios::Register(registry);
  CompressedFileScenarios::Register(registry);
  EfsFixtureScenarios::Register(registry);
  DataRunLcnScenarios::Register(registry);
  LegacyNameScenarios::Register(registry);
  CorpusTraceScenarios::Register(registry);
  return registry;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
