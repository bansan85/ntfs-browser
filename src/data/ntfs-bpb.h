#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <string_view>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

// NTFS Boot Sector BPB

// OEM signature of an NTFS boot sector: "NTFS" padded with spaces to
// bpb_signature_size bytes.
inline constexpr std::string_view ntfs_signature = "NTFS    ";

// Size of the OEM signature field.
inline constexpr size_t bpb_signature_size = 8;

// Size of the volume serial number field.
inline constexpr size_t bpb_volume_serial_size = 8;

// Size of the boot code that fills the sector up to its 0xAA55 marker.
inline constexpr size_t bpb_boot_code_size = 430;

// Byte 510 of the boot sector: the low byte of the end marker WORD 0xAA55,
// which little-endian storage puts first.
inline constexpr BYTE bpb_marker_first = 0x55;

// Byte 511 of the boot sector: the high byte of the end marker WORD 0xAA55.
inline constexpr BYTE bpb_marker_second = 0xAA;

#pragma pack(1)

struct NtfsBpb {
  // jump instruction
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE jmp[3];

  // signature
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE signature[bpb_signature_size];

  // BPB and extended BPB
  WORD bytes_per_sector;
  BYTE sectors_per_cluster;
  WORD reserved_sectors;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE zeros1[3];
  WORD not_used1;
  BYTE media_descriptor;
  WORD zeros2;
  WORD sectors_per_track;
  WORD number_of_heads;
  DWORD hidden_sectors;
  DWORD not_used2;
  DWORD not_used3;
  ULONGLONG total_sectors;
  ULONGLONG lcn_mft;
  ULONGLONG lcn_mft_mirr;
  DWORD clusters_per_file_record;
  DWORD clusters_per_index_block;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE volume_sn[bpb_volume_serial_size];

  // boot code
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE code[bpb_boot_code_size];

  // End marker 0xAA55. As stored, x_aa (byte 510) is 0x55 and x_55 (byte
  // 511) is 0xAA: the names read the WORD, not the bytes.
  BYTE x_aa;
  BYTE x_55;
};

#pragma pack()

};  // namespace NTFS_BROWSER_NAMESPACE::Data
