#pragma once

#include <ntfs-browser/win-types.h>

#include <span>

namespace NtfsBrowser::Volume {

// Sizes and addresses the boot sector's BPB declares, in bytes.
struct Geometry {
  WORD sector_size{0};
  DWORD cluster_size{0};
  DWORD file_record_size{0};
  DWORD index_block_size{0};
  ULONGLONG mft_addr{0};
};

// Result of ParseBootSector(). geometry keeps every field decoded before the
// first rejection; the fields after it stay zero.
struct BootSectorParse {
  Geometry geometry;
  bool ok{false};
};

// Decodes the geometry from a boot sector, logging each field as it goes.
// boot_sector MUST hold at least one whole BPB, 512 bytes.
[[nodiscard]] BootSectorParse
    ParseBootSector(std::span<const BYTE> boot_sector);

}  // namespace NtfsBrowser::Volume
