#pragma once

#include <ntfs-browser/win-types.h>

#include <functional>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

struct NtfsBpb;

}  // namespace NTFS_BROWSER_NAMESPACE::Data

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Edits the fields of a boot sector (the BPB) by name, so that a damaged boot
// sector reads as the field that is wrong, not as a table of offsets.
class BpbEdit {
 public:
  // A change to the fields of a boot sector.
  using Change = std::function<void(NTFS_BROWSER_NAMESPACE::Data::NtfsBpb&)>;

  // The boot sector of the base volume (BuildFakeNtfsImage()): 512 bytes.
  [[nodiscard]] static std::vector<BYTE> BaseBootSector();

  // Applies change to the boot sector that starts at the first byte of bytes.
  // Throws std::out_of_range when bytes is shorter than a boot sector.
  static void Apply(std::vector<BYTE>& bytes, const Change& change);

  // Bytes of a boot sector, whatever the sector size.
  static constexpr std::size_t boot_sector_bytes = 512;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
