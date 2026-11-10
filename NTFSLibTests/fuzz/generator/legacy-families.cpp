#include "legacy-families.h"

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "legacy-attribute-list-scenarios.h"
#include "legacy-builder-scenarios.h"
#include "legacy-index-block-scenarios.h"
#include "legacy-index-root-scenarios.h"
#include "legacy-name-scenarios.h"
#include "legacy-residency-scenarios.h"
#include "legacy-root-record-scenarios.h"
#include "legacy-volume-scenarios.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void LegacyFamilies::Register(ScenarioRegistry& registry) {
  LegacyIndexRootScenarios::Register(registry);
  LegacyRootRecordScenarios::Register(registry);
  LegacyBuilderScenarios::Register(registry);
  LegacyAttributeListScenarios::Register(registry);
  LegacyIndexBlockScenarios::Register(registry);
  LegacyResidencyScenarios::Register(registry);
  LegacyVolumeScenarios::Register(registry);
  LegacyNameScenarios::Register(registry);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
