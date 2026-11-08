#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The traces of corpus-traces that a corpus test records from a forensic
// image: each is made by running the probe of the test on a sparse image that
// the resource archive of the corpus describes.
class CorpusTraceScenarios {
 public:
  // Adds the traces of the family.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
