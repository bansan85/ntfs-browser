#pragma once

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <span>
#include <string_view>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Writes generated files under an output folder.
class ByteWriter {
 public:
  // folder is created, with its parents, when the first file is written.
  explicit ByteWriter(std::filesystem::path folder);

  // Writes bytes to the file called name under the folder, replacing it. name
  // may hold forward slashes: the subfolders are created. Throws
  // std::runtime_error when the file cannot be written.
  void Write(std::string_view name, std::span<const BYTE> bytes) const;

 private:
  std::filesystem::path folder_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
