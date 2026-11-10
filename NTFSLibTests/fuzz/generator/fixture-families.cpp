#include "fixture-families.h"

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "attribute-list-fixture-scenarios.h"
#include "file-record-scenarios.h"
#include "index-fixture-scenarios.h"
#include "mft-fixture-scenarios.h"
#include "volume-record-scenarios.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void FixtureFamilies::Register(ScenarioRegistry& registry) {
  MftFixtureScenarios::Register(registry);
  FileRecordScenarios::Register(registry);
  VolumeRecordScenarios::Register(registry);
  AttributeListFixtureScenarios::Register(registry);
  IndexFixtureScenarios::Register(registry);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
