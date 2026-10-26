#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// What the bytes of one cluster look like.
enum class ClusterContent : std::uint8_t {
  // Every byte is zero: TRIM, a wipe, or a cluster that was never written.
  Zeros,
  // A sector of the cluster starts with FILE, INDX, RCRD or RSTR: NTFS wrote
  // its own metadata there.
  Metadata,
  // Anything else: it may well be the file's own bytes.
  Data
};

// Tells what cluster holds. Zeros wins over Metadata, which wins over Data. A
// signature is looked for at the start of every sector of sector_size bytes
// (512 when 0), and a trailing part of a cluster shorter than a sector is
// ignored.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY ClusterContent ClassifyCluster(
    std::span<const BYTE> cluster, std::size_t sector_size) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
