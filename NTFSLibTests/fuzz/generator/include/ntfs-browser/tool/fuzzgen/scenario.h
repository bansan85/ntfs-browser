#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

class ResourceLocator;

// One generated file.
struct Scenario {
  // The path of the file under the output folder, with forward slashes.
  std::string name;
  // The feature that the build MUST have to make the file.
  Feature needs = Feature::None;
  // Makes the bytes of the file. It reads the extracted resources through the
  // locator and throws when it cannot make them.
  std::function<std::vector<BYTE>(const ResourceLocator&)> make;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
