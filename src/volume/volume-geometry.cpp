#include "volume/volume-geometry.h"

#include <ntfs-browser/win-types.h>

#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/ntfs-bpb.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Volume {

namespace {

// Largest -log2 of sectors per cluster the BPB encoding may carry: a cluster
// of 2^12 sectors, far beyond any real volume.
constexpr int max_sectors_per_cluster_shift = 12;

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

// Computes the cluster size from the sectors-per-cluster BPB byte, or nullopt
// when its magnitude is out of range.
std::optional<DWORD> DecodeClusterSize(char spc, WORD sector_size) {
  if (spc >= 0) {
    return sector_size * static_cast<unsigned char>(spc);
  }
  // Windows 10 1903+ (build 18362) large-cluster encoding: a negative byte
  // is -log2(sectors per cluster), not a literal sector count, letting a
  // single BYTE field reach cluster sizes above 255 sectors (up to 2 MiB
  // at the common 512-byte sector size).
  if (spc < -max_sectors_per_cluster_shift) {
    return std::nullopt;
  }
  return sector_size * (1U << static_cast<unsigned char>(-spc));
}

// Set sector_size from the BPB. Reject a sector too small to be real.
bool ParseSectorSize(const Data::NtfsBpb& bpb, Geometry& geometry) {
  geometry.sector_size = bpb.bytes_per_sector;
  Log::Info("Sector Size = {} bytes", geometry.sector_size);

  // A sector smaller than one WORD cannot be a real BPB value.
  if (geometry.sector_size < sizeof(WORD)) {
    Log::Error("Sector Size must be at least 2 bytes");
    return false;
  }
  return true;
}

// Set cluster_size from the BPB. Reject a null cluster.
bool ParseClusterSize(const Data::NtfsBpb& bpb, Geometry& geometry) {
  const std::optional<DWORD> decoded_cluster_size = DecodeClusterSize(
      static_cast<char>(bpb.sectors_per_cluster), geometry.sector_size);
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

}  // namespace

BootSectorParse ParseBootSector(std::span<const BYTE> boot_sector) {
  BootSectorParse parse;
  const auto& bpb = *reinterpret_cast<const Data::NtfsBpb*>(boot_sector.data());

  const std::string_view signature(
      reinterpret_cast<const char*>(&bpb.signature[0]), sizeof(bpb.signature));
  if (signature != Data::ntfs_signature) {
    Log::Warn("Volume file system is not NTFS");
    return parse;
  }

  // Each field is validated and logged in turn. The first failure stops the
  // rest, so later fields are neither set nor logged.
  parse.ok = ParseSectorSize(bpb, parse.geometry) &&
             ParseClusterSize(bpb, parse.geometry) &&
             ParseFileRecordSize(bpb, parse.geometry) &&
             ParseIndexBlockSize(bpb, parse.geometry) &&
             ParseMftAddress(bpb, parse.geometry);
  return parse;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Volume
