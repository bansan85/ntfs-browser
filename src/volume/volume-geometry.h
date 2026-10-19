#pragma once

#include <ntfs-browser/win-types.h>

#include <span>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Volume {

// Records $MFTMirr copies: $MFT, $MFTMirr, $LogFile and $Volume.
inline constexpr ULONGLONG mft_mirror_record_count = 4;

// Sizes and addresses the boot sector's BPB declares, in bytes.
struct Geometry {
  WORD sector_size{0};
  DWORD cluster_size{0};
  DWORD file_record_size{0};
  DWORD index_block_size{0};
  ULONGLONG mft_addr{0};
  // lcn_mft_mirr in bytes, saturated when the product overflows. Decoded
  // whenever the whole parse is accepted, usable or not.
  ULONGLONG mft_mirr_addr{0};
  // True when a copy of the first mft_mirror_record_count records can be read
  // at mft_mirr_addr: see ParseBootSector().
  bool mft_mirr_usable{false};
};

// Result of ParseBootSector(). geometry keeps every field decoded before the
// first rejection; the fields after it stay zero.
struct BootSectorParse {
  Geometry geometry;
  bool ok{false};
};

// Decodes the geometry from a boot sector, logging each field as it goes.
// boot_sector MUST hold at least one whole BPB, 512 bytes. A missing 0xAA55
// end marker rejects the sector, unless recover is true: it is then logged
// and the parse goes on. The same holds for a FAT-era BPB field a real NTFS
// volume keeps at zero (reserved_sectors, the FAT count and root entries, the
// small sector count, sectors_per_fat, the large sector count), for a null
// total_sectors, and for a volume too small to hold the first $MFT record. The
// last check needs the geometry, so it runs after the others. A sector size
// outside 256..4096 bytes or not a power of two, and a cluster size that is not
// a power of two, always reject: that geometry cannot be salvaged.
// Once the parse is accepted, lcn_mft_mirr is decoded too. It never rejects:
// an unusable $MFTMirr only leaves mft_mirr_usable false. It is unusable when
// null (the boot sector sits at LCN 0), when its address overflows or is too
// high for a LONGLONG, when it equals mft_addr, or when the copy would end
// beyond total_sectors. A null total_sectors, only accepted when recovering,
// leaves the volume's end unknown: that last check is then skipped.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY BootSectorParse
    ParseBootSector(std::span<const BYTE> boot_sector, bool recover);

}  // namespace NTFS_BROWSER_NAMESPACE::Volume
