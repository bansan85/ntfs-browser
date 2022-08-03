#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>

#include "../flag/file-record.h"

namespace NtfsBrowser
{
constexpr uint32_t kFileRecordMagic('ELIF');

struct FileRecordHeader
{
  union
  {
    struct
    {
      DWORD Magic;        // "FILE"
      WORD OffsetOfUS;    // Offset of Update Sequence
      WORD SizeOfUS;      // Size in words of Update Sequence Number & Array
      ULONGLONG LSN;      // $LogFile Sequence Number
      WORD SeqNo;         // Sequence number
      WORD Hardlinks;     // Hard link count
      WORD OffsetOfAttr;  // Offset of the first Attribute
      Flag::FileRecord Flags;  // Flags
      DWORD RealSize;          // Real size of the FILE record
      DWORD AllocSize;         // Allocated size of the FILE record
      ULONGLONG RefToBase;     // File reference to the base FILE record
      WORD NextAttrId;         // Next Attribute Id
      WORD Align;              // Align to 4 byte boundary
      DWORD RecordNo;          // Number of this MFT Record
    };
    BYTE Raw[1024];
  };

  WORD USNumber;
  std::vector<WORD> USArray;
  size_t sectorSize;

  FileRecordHeader(BYTE* buffer, size_t fileRecordSize, size_t sectorSize)
  {
    this->sectorSize = sectorSize;
    _ASSERT(1024 == fileRecordSize);
    memcpy(&Raw[0], buffer, fileRecordSize);

    if (Magic == kFileRecordMagic)
    {
      USArray.reserve(fileRecordSize / sectorSize);
      WORD* usnaddr = (WORD*)((BYTE*)buffer + OffsetOfUS);
      USNumber = *usnaddr;
      WORD* usarray = usnaddr + 1;

      for (WORD i = 0; i < fileRecordSize / sectorSize; i++)
        USArray.push_back(usarray[i]);
    }
    else
    {
      USNumber = 0;
    }
  }

  // Verify US and update sectors
  BOOL PatchUS()
  {
    WORD* sector = (WORD*)&Magic;
    for (size_t i = 0; i < USArray.size(); i++)
    {
      sector += ((sectorSize >> 1) - 1);
      if (*sector != USNumber) return FALSE;  // USN error
      *sector = USArray[i];                   // Write back correct data
      sector++;
    }
    return TRUE;
  }

  const AttrHeaderCommon* HeaderCommon()
  {
    return (AttrHeaderCommon*)(Raw + OffsetOfAttr);
  }
};
}  // namespace NtfsBrowser