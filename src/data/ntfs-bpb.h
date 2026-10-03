#pragma once

#include <ntfs-browser/win-types.h>

#include <string_view>

namespace NtfsBrowser::Data
{

// NTFS Boot Sector BPB

// OEM signature of an NTFS boot sector: "NTFS" padded with spaces to
// kBpbSignatureSize bytes.
inline constexpr std::string_view kNtfsSignature = "NTFS    ";

// Size of the OEM signature field.
inline constexpr size_t kBpbSignatureSize = 8;

// Size of the volume serial number field.
inline constexpr size_t kBpbVolumeSerialSize = 8;

// Size of the boot code that fills the sector up to its 0xAA55 marker.
inline constexpr size_t kBpbBootCodeSize = 430;

#pragma pack(1)
struct NtfsBpb
{
  // jump instruction
  BYTE jmp[3];

  // signature
  BYTE signature[kBpbSignatureSize];

  // BPB and extended BPB
  WORD bytes_per_sector;
  BYTE sectors_per_cluster;
  WORD reserved_sectors;
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
  BYTE volume_sn[kBpbVolumeSerialSize];

  // boot code
  BYTE code[kBpbBootCodeSize];

  //0xAA55
  BYTE x_aa;
  BYTE x_55;
};
#pragma pack()

};  // namespace NtfsBrowser::Data