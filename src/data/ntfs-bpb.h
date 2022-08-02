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
  BYTE Jmp[3];

  // signature
  BYTE Signature[8];

  // BPB and extended BPB
  WORD BytesPerSector;
  BYTE SectorsPerCluster;
  WORD ReservedSectors;
  BYTE Zeros1[3];
  WORD NotUsed1;
  BYTE MediaDescriptor;
  WORD Zeros2;
  WORD SectorsPerTrack;
  WORD NumberOfHeads;
  DWORD HiddenSectors;
  DWORD NotUsed2;
  DWORD NotUsed3;
  ULONGLONG TotalSectors;
  ULONGLONG LCN_MFT;
  ULONGLONG LCN_MFTMirr;
  DWORD ClustersPerFileRecord;
  DWORD ClustersPerIndexBlock;
  BYTE VolumeSN[8];

  // boot code
  BYTE Code[430];

  //0xAA55
  BYTE _AA;
  BYTE _55;
};
#pragma pack()

};  // namespace NtfsBrowser::Data