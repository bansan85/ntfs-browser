#pragma once

#include <cstdint>
#include <vector>

#include <gsl/pointers>

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
      DWORD magic;        // "FILE"
      WORD offset_of_us;  // Offset of Update Sequence
      WORD size_of_us;    // Size in words of Update Sequence Number & Array
      ULONGLONG lsn;      // $LogFile Sequence Number
      WORD SeqNo;         // Sequence number
      WORD Hardlinks;     // Hard link count
      WORD OffsetOfAttr;  // Offset of the first Attribute
      Flag::FileRecord flags;  // Flags
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

    if (magic == kFileRecordMagic)
    {
      USArray.reserve(fileRecordSize / sectorSize);
      const gsl::not_null<WORD*> usnaddr =
          reinterpret_cast<WORD*>(buffer + offset_of_us);
      USNumber = *usnaddr;
      const gsl::not_null<WORD*> usarray = usnaddr.get() + 1;

      for (WORD i = 0; i < fileRecordSize / sectorSize; i++)
        USArray.push_back(usarray.get()[i]);
    }
    else
    {
      USNumber = 0;
    }
  }

  // Verify US and update sectors
  BOOL PatchUS() noexcept
  {
    gsl::not_null<WORD*> sector = reinterpret_cast<WORD*>(&Raw[0]);
    for (WORD value : USArray)
    {
      sector = sector.get() + ((sectorSize >> 1) - 1);
      // USN error
      if (*sector != USNumber)
      {
        return FALSE;
      }
      // Write back correct data
      *sector = value;
      sector = sector.get() + 1;
    }
    return TRUE;
  }

  const AttrHeaderCommon* HeaderCommon() noexcept
  {
    return reinterpret_cast<const AttrHeaderCommon*>(Raw + OffsetOfAttr);
  }
};
}  // namespace NtfsBrowser