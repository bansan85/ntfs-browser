#pragma once

#include <cstdint>
#include <string_view>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// A library feature that a build may leave out. A scenario that needs one is
// skipped by a build without it.
enum class Feature : std::uint8_t {
  None,
  // LZNT1 decompression (NTFS_BROWSER_ENABLE_DECOMPRESSION).
  Decompression,
  // An EFS cipher backend (CryptoPP, or BCrypt on Windows).
  Efs
};

// What the build of the generator contains.
class BuildFeatures {
 public:
  // True when this build has feature. Feature::None is always present.
  [[nodiscard]] static bool Has(Feature feature);

  // The name of feature, for a message.
  [[nodiscard]] static std::string_view Name(Feature feature);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
