#pragma once

#include <cstdint>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The volume geometry of the first fake images: one sector of 1024 bytes per
// cluster and per file record. The fake builders of today use 512-byte
// sectors, two per cluster. The cluster has the same size, so every address
// stays. Only the boot sector and the update sequence arrays change.
class LegacyGeometry {
 public:
  // The two eras of the old builders. They differ in the $VOLUME_INFORMATION
  // of the volume record.
  enum class Era : std::uint8_t {
    // Until 2026-09-17: a value of 16 bytes, in an attribute of 40 bytes.
    Early,
    // After the fix that accepts the real 12-byte value: an attribute of 36
    // bytes, with no padding.
    Late
  };

  // Gives the boot sector of image the old sector size, and every file record
  // and index block the one-sector update sequence array it had then. A
  // structure that does not have the layout of the current builders is left
  // alone, whatever its header says. The volume record takes the form of era.
  static void Apply(std::vector<BYTE>& image, Era era);

  // The old builders gave an attribute the exact size of its header and its
  // body, with no padding to 8 bytes. Cuts the last attribute of record idx
  // back to that size and moves the end marker up. Does nothing when the
  // record has no attribute.
  static void PackLastAttribute(std::vector<BYTE>& image, ULONGLONG idx);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
