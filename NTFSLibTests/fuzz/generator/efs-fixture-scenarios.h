#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs about EFS: an encrypted file of the fake volume whose stream a
// // cipher of the unit tests encrypted, and records with a damaged $EFS
// stream.
// // They need an EFS backend in the build.
class EfsFixtureScenarios {
 public:
  // Adds the inputs of the family.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
