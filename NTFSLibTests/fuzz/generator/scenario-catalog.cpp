#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/alloc-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/attribute-list-fixture-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/attribute-list-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/bad-cluster-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/boot-sector-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/case-twin-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/cluster-bitmap-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/cluster-state-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/compressed-file-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/corpus-trace-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/data-run-lcn-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/efs-fixture-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/file-record-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/index-fixture-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/index-slack-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-attribute-list-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-builder-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-index-block-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-index-root-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-name-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-residency-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-root-record-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-volume-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/mft-fixture-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/mft-huge-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/mft-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/record-slack-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/run-list-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/scenario-catalog.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/slack-names-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/tiny-stream-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/usn-journal-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/usn-tree-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/volume-record-scenarios.h>

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
