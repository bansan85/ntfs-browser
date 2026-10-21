#include "volume/volume-geometry.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/ntfs-bpb.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Volume {

namespace {

// Smallest sector size NTFS supports, in bytes.
constexpr WORD min_sector_size = 256;

// Largest sector size NTFS supports, in bytes: the 4Kn drive sector.
constexpr WORD max_sector_size = 4096;

// Largest -log2 of sectors per cluster the BPB encoding may carry: a cluster
// of 2^12 sectors, far beyond any real volume.
constexpr unsigned max_sectors_per_cluster_shift = 12;

// Largest -log2 of a file record or index block size, in bytes.
constexpr int max_size_shift = 12;

// Largest literal cluster count of a file record or index block.
constexpr int max_size_in_clusters = 8;

// Decodes a BPB size byte: a positive value counts clusters, a negative one is
// -log2 of the size in bytes. Rejects a magnitude that would shift 1U by 32 or
// more (undefined behaviour), or yield a size no real volume could have.
std::optional<DWORD> DecodeBpbSize(char raw, DWORD cluster_size) {
  if (raw < -max_size_shift || raw > max_size_in_clusters) {
    return std::nullopt;
  }
  if (raw > 0) {
    return cluster_size * raw;
  }
  return 1U << static_cast<unsigned char>(-raw);
}

// Largest sectors_per_cluster byte that counts sectors: 0x80, which is 128
// sectors. Windows writes it for a 64 KiB cluster on 512-byte sectors. As a
// signed byte it would be -128, a shift the negative encoding does not use.
constexpr BYTE max_literal_sectors_per_cluster = 0x80;

// Computes the cluster size from the sectors-per-cluster BPB byte, or nullopt
// when its magnitude is out of range.
std::optional<DWORD> DecodeClusterSize(BYTE spc, WORD sector_size) {
  if (spc <= max_literal_sectors_per_cluster) {
    return sector_size * spc;
  }
  // Windows 10 1903+ (build 18362) large-cluster encoding: a byte above 0x80
  // is -log2(sectors per cluster) as a signed byte, not a literal sector
  // count, letting a single BYTE field reach cluster sizes above 128 sectors
  // (up to 2 MiB at the common 512-byte sector size).
  const unsigned shift = 256U - spc;
  if (shift > max_sectors_per_cluster_shift) {
    return std::nullopt;
  }
  return sector_size * (1U << shift);
}

// Checks the BPB fields NTFS inherits from FAT, which a real volume keeps at
// zero: reserved_sectors, the FAT count and root entries, the 16-bit sector
// count, sectors_per_fat and the 32-bit sector count. A non-zero field is one
// message naming each offender, kept when recover is true. not_used3 is
// not checked: many volumes hold 0x80008000 there.
bool ParseReservedFields(const Data::NtfsBpb& bpb, bool recover) {
  const auto all_zero = [](const auto& field) {
    const auto* bytes = reinterpret_cast<const BYTE*>(&field);
    return std::ranges::all_of(std::span(bytes, sizeof(field)),
                               [](BYTE byte) { return byte == 0; });
  };

  std::string offenders;
  const auto note = [&](const auto& field, std::string_view name) {
    if (!all_zero(field)) {
      offenders += offenders.empty() ? "" : ", ";
      offenders += name;
    }
  };
  note(bpb.reserved_sectors, "reserved_sectors (0x0E)");
  note(bpb.zeros1, "fat_count_and_root_entries (0x10)");
  note(bpb.not_used1, "small_sectors (0x13)");
  note(bpb.zeros2, "sectors_per_fat (0x16)");
  note(bpb.not_used2, "large_sectors (0x20)");

  if (offenders.empty()) {
    return true;
  }
  Log::Recoverable(recover, "Boot sector fields that must be zero are not: {}",
                   offenders);
  return recover;
}

// Set sector_size from the BPB. Reject a size outside 256..4096 bytes, and a
// size that is not a power of two. The geometry is not salvageable.
bool ParseSectorSize(const Data::NtfsBpb& bpb, Geometry& geometry) {
  geometry.sector_size = bpb.bytes_per_sector;
  Log::Info("Sector Size = {} bytes", geometry.sector_size);

  if (geometry.sector_size < min_sector_size ||
      geometry.sector_size > max_sector_size) {
    Log::Error("Sector Size must be between {} and {} bytes", min_sector_size,
               max_sector_size);
    return false;
  }
  if (!std::has_single_bit(geometry.sector_size)) {
    Log::Error("Sector Size must be a power of two");
    return false;
  }
  return true;
}

// Set cluster_size from the BPB. Reject a null cluster, and a cluster that is
// not a power of two. The sector size is one, so this means a positive
// sectors_per_cluster that is not.
bool ParseClusterSize(const Data::NtfsBpb& bpb, Geometry& geometry) {
  const std::optional<DWORD> decoded_cluster_size =
      DecodeClusterSize(bpb.sectors_per_cluster, geometry.sector_size);
  if (!decoded_cluster_size) {
    Log::Error("sectors_per_cluster magnitude out of range");
    return false;
  }
  geometry.cluster_size = *decoded_cluster_size;
  Log::Info("Cluster Size = {} bytes", geometry.cluster_size);

  if (geometry.cluster_size == 0) {
    Log::Error("Cluster Size can't be null");
    return false;
  }

  if (!std::has_single_bit(geometry.cluster_size)) {
    Log::Error("Cluster Size must be a power of two");
    return false;
  }
  return true;
}

// Set file_record_size from the BPB. Reject a size that is not a usable record.
bool ParseFileRecordSize(const Data::NtfsBpb& bpb, Geometry& geometry) {
  const std::optional<DWORD> decoded_record_size = DecodeBpbSize(
      static_cast<char>(bpb.clusters_per_file_record), geometry.cluster_size);
  if (!decoded_record_size) {
    Log::Error("clusters_per_file_record magnitude out of range");
    return false;
  }
  geometry.file_record_size = *decoded_record_size;
  Log::Info("FileRecord Size = {} bytes", geometry.file_record_size);

  // Rejects a size too small for the header, or not a whole number of
  // sectors.
  if (geometry.file_record_size <
          Data::FileRecordHeader::min_file_record_header_size ||
      geometry.file_record_size % geometry.sector_size != 0) {
    Log::Error("FileRecord Size is invalid");
    return false;
  }

  if (geometry.file_record_size >
      Data::FileRecordHeader::max_file_record_size) {
    Log::Error(
        "FileRecord Size exceeds the maximum supported file record size");
    return false;
  }
  return true;
}

// Set index_block_size from the BPB. Reject a size that is not a usable block.
bool ParseIndexBlockSize(const Data::NtfsBpb& bpb, Geometry& geometry) {
  const std::optional<DWORD> decoded_index_block_size = DecodeBpbSize(
      static_cast<char>(bpb.clusters_per_index_block), geometry.cluster_size);
  if (!decoded_index_block_size) {
    Log::Error("clusters_per_index_block magnitude out of range");
    return false;
  }
  geometry.index_block_size = *decoded_index_block_size;
  Log::Info("IndexBlock Size = {} bytes", geometry.index_block_size);

  // Rejects a size too small for the header, or not a whole number of
  // sectors.
  if (geometry.index_block_size < sizeof(Data::IndexBlock) ||
      geometry.index_block_size % geometry.sector_size != 0) {
    Log::Error("IndexBlock Size is invalid");
    return false;
  }
  return true;
}

// Set mft_addr from the BPB. Reject an address that overflows or is too high.
bool ParseMftAddress(const Data::NtfsBpb& bpb, Geometry& geometry) {
  // Multiplying two attacker-controlled values can overflow mft_addr's type.
  const bool mft_addr_overflows =
      geometry.cluster_size != 0 &&
      bpb.lcn_mft >
          std::numeric_limits<ULONGLONG>::max() / geometry.cluster_size;
  geometry.mft_addr = mft_addr_overflows ? std::numeric_limits<ULONGLONG>::max()
                                         : bpb.lcn_mft * geometry.cluster_size;
  Log::Info("MFT address = 0x{:016X}", geometry.mft_addr);

  // Leaves headroom for the per-record byte offset added to mft_addr
  // later, before it is narrowed to a LONGLONG.
  constexpr ULONGLONG max_plausible_mft_addr =
      std::numeric_limits<LONGLONG>::max() / 2;

  if (mft_addr_overflows || geometry.mft_addr > max_plausible_mft_addr) {
    Log::Error("MFT address is invalid");
    return false;
  }
  return true;
}

// Size of the volume in bytes. Saturates instead of overflowing: both factors
// come from the BPB.
ULONGLONG VolumeBytes(const Data::NtfsBpb& bpb, const Geometry& geometry) {
  return bpb.total_sectors >
                 std::numeric_limits<ULONGLONG>::max() / geometry.sector_size
             ? std::numeric_limits<ULONGLONG>::max()
             : bpb.total_sectors * geometry.sector_size;
}

// Check total_sectors from the BPB. Reject a null count, and a volume too
// small to hold the first $MFT record. A salvageable condition is logged and
// kept when recover is true.
bool ParseTotalSectors(const Data::NtfsBpb& bpb, const Geometry& geometry,
                       bool recover) {
  Log::Info("Total sectors = {}", bpb.total_sectors);

  if (bpb.total_sectors == 0) {
    Log::Recoverable(recover, "Total sectors can't be null");
    // A null volume holds no record at all: the size check adds nothing.
    return recover;
  }

  const ULONGLONG volume_bytes = VolumeBytes(bpb, geometry);

  // The first $MFT record, at mft_addr, MUST end inside the volume. The
  // subtraction cannot wrap, since the first operand is checked first.
  if (geometry.mft_addr > volume_bytes ||
      geometry.file_record_size > volume_bytes - geometry.mft_addr) {
    Log::Recoverable(recover,
                     "First $MFT record lies beyond the volume's {} sectors",
                     bpb.total_sectors);
    return recover;
  }
  return true;
}

// Set mft_mirr_addr from the BPB, and decide whether a copy of the first $MFT
// records can be read there. Never rejects: an unusable address only leaves
// mft_mirr_usable false.
void ParseMftMirrorAddress(const Data::NtfsBpb& bpb, Geometry& geometry) {
  // Multiplying two attacker-controlled values can overflow mft_mirr_addr's
  // type.
  const bool overflows =
      bpb.lcn_mft_mirr >
      std::numeric_limits<ULONGLONG>::max() / geometry.cluster_size;
  geometry.mft_mirr_addr = overflows ? std::numeric_limits<ULONGLONG>::max()
                                     : bpb.lcn_mft_mirr * geometry.cluster_size;
  Log::Info("MFTMirr address = 0x{:016X}", geometry.mft_mirr_addr);

  // Leaves headroom for the per-record byte offset added to mft_mirr_addr
  // later, like mft_addr.
  constexpr ULONGLONG max_plausible_addr =
      std::numeric_limits<LONGLONG>::max() / 2;
  const ULONGLONG copy_bytes =
      mft_mirror_record_count * ULONGLONG{geometry.file_record_size};
  const ULONGLONG volume_bytes = VolumeBytes(bpb, geometry);
  // Only a null total_sectors leaves the volume's size unknown. The
  // subtraction cannot wrap, since the first operand is checked first.
  const bool inside_volume =
      bpb.total_sectors == 0 ||
      (geometry.mft_mirr_addr <= volume_bytes &&
       copy_bytes <= volume_bytes - geometry.mft_mirr_addr);

  // The mirror MUST sit apart from the boot sector (LCN 0) and from $MFT, and
  // fit in the volume, with an address that stays a plausible LONGLONG.
  geometry.mft_mirr_usable = bpb.lcn_mft_mirr != 0 && !overflows &&
                             geometry.mft_mirr_addr <= max_plausible_addr &&
                             geometry.mft_mirr_addr != geometry.mft_addr &&
                             inside_volume;
  if (!geometry.mft_mirr_usable) {
    Log::Debug("No usable $MFTMirr: records 0 to 3 have no fallback copy");
  }
}

}  // namespace

BootSectorParse ParseBootSector(std::span<const BYTE> boot_sector,
                                bool recover) {
  BootSectorParse parse;
  const auto& bpb = *reinterpret_cast<const Data::NtfsBpb*>(boot_sector.data());

  const std::string_view signature(
      reinterpret_cast<const char*>(&bpb.signature[0]), sizeof(bpb.signature));
  if (signature != Data::ntfs_signature) {
    Log::Warn("Volume file system is not NTFS");
    return parse;
  }

  if (bpb.x_aa != Data::bpb_marker_first ||
      bpb.x_55 != Data::bpb_marker_second) {
    Log::Recoverable(recover, "Boot sector end marker is not 0xAA55");
    if (!recover) {
      return parse;
    }
  }

  if (!ParseReservedFields(bpb, recover)) {
    return parse;
  }

  // Each field is validated and logged in turn. The first failure stops the
  // rest, so later fields are neither set nor logged.
  parse.ok = ParseSectorSize(bpb, parse.geometry) &&
             ParseClusterSize(bpb, parse.geometry) &&
             ParseFileRecordSize(bpb, parse.geometry) &&
             ParseIndexBlockSize(bpb, parse.geometry) &&
             ParseMftAddress(bpb, parse.geometry) &&
             ParseTotalSectors(bpb, parse.geometry, recover);
  if (parse.ok) {
    ParseMftMirrorAddress(bpb, parse.geometry);
    parse.geometry.cluster_count =
        bpb.total_sectors == 0
            ? 0
            : VolumeBytes(bpb, parse.geometry) / parse.geometry.cluster_size;
  }
  return parse;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Volume
