#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Finds the plain files that the build extracted from the resource archives
// (the parts of a recorded input that code cannot compute). The folder holds
// one subfolder per archive.
class ResourceLocator {
 public:
  // root is the folder the archives were extracted into. It may be empty or
  // missing: a scenario that needs a file then fails when it asks for it.
  explicit ResourceLocator(std::filesystem::path root);

  // The folder the archives were extracted into.
  [[nodiscard]] const std::filesystem::path& Root() const;

  // The path of the resource called name, with forward slashes, under Root().
  [[nodiscard]] std::filesystem::path Path(std::string_view name) const;

  // True when the resource called name exists.
  [[nodiscard]] bool Has(std::string_view name) const;

  // The bytes of the resource called name. Throws std::runtime_error, with the
  // path it looked at, when the resource is missing or cannot be read.
  [[nodiscard]] std::vector<BYTE> Read(std::string_view name) const;

 private:
  std::filesystem::path root_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
