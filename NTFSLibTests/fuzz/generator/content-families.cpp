#include "content-families.h"

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "compressed-file-scenarios.h"
#include "corpus-trace-scenarios.h"
#include "data-run-lcn-scenarios.h"
#include "efs-fixture-scenarios.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void ContentFamilies::Register(ScenarioRegistry& registry) {
  CompressedFileScenarios::Register(registry);
  EfsFixtureScenarios::Register(registry);
  DataRunLcnScenarios::Register(registry);
  CorpusTraceScenarios::Register(registry);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
