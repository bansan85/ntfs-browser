#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Every scenario the generator knows.
class ScenarioCatalog {
 public:
  // A registry that holds the scenarios of each family.
  [[nodiscard]] static ScenarioRegistry Build();
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
