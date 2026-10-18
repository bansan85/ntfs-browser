#pragma once

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Cache {

// Proves the caller is the library: Cache::View declares this type but never
// defines it, and this header is not installed. Only library code can build
// the key that View's populated constructor asks for.
class ViewKey {
 public:
  ViewKey() noexcept = default;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Cache
