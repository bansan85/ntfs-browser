#pragma once

#include <cstdint>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Cache {

enum class Strategy : std::uint8_t { NoCache, FullCache };

}  // namespace NTFS_BROWSER_NAMESPACE::Cache
