#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-catalog.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "content-families.h"
#include "fixture-families.h"
#include "legacy-families.h"
#include "scan-input-families.h"
#include "volume-state-families.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

ScenarioRegistry ScenarioCatalog::Build() {
  ScenarioRegistry registry;
  VolumeStateFamilies::Register(registry);
  ScanInputFamilies::Register(registry);
  FixtureFamilies::Register(registry);
  LegacyFamilies::Register(registry);
  ContentFamilies::Register(registry);
  return registry;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
