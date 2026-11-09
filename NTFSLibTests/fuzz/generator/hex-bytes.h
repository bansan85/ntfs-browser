#pragma once

#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Bytes written as hexadecimal text, for the few inputs that no structure
// describes.
class HexBytes {
 public:
  // The bytes that text spells, two digits each. Spaces are skipped. Throws
  // std::invalid_argument on any other character or on an odd digit count.
  [[nodiscard]] static std::vector<BYTE> Decode(std::string_view text);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
