#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

bool BuildFeatures::Has(Feature feature) {
  switch (feature) {
    case Feature::None:
      return true;
    case Feature::Decompression:
#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
      return true;
#else
      return false;
#endif
    case Feature::Efs:
#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
      return true;
#else
      return false;
#endif
  }
  return false;
}

std::string_view BuildFeatures::Name(Feature feature) {
  switch (feature) {
    case Feature::None:
      return "none";
    case Feature::Decompression:
      return "decompression";
    case Feature::Efs:
      return "efs";
  }
  return "unknown";
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
