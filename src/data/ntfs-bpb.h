#pragma once

#include <windows.h>

namespace NtfsBrowser::Data
{

// NTFS Boot Sector BPB

#define NTFS_SIGNATURE "NTFS    "

#pragma pack(1)
struct NtfsBpb
{
  // jump instruction
  BYTE jmp[3];

  // signature
  BYTE signature[8];

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
  BYTE volume_sn[8];

  // boot code
  BYTE code[430];

  //0xAA55
  BYTE x_aa;
  BYTE x_55;
};
#pragma pack()

};  // namespace NtfsBrowser::Data