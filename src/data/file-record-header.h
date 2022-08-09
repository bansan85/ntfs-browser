#pragma once

#include <cstdint>
#include <vector>

#include <gsl/pointers>

#include <windows.h>

#include "../flag/file-record.h"

// OK

namespace NtfsBrowser
{
constexpr uint32_t kFileRecordMagic('ELIF');

struct FileRecordHeader
{
  union
  {
    struct
    {
      DWORD magic;          // "FILE"
      WORD offset_of_us;    // Offset of Update Sequence
      WORD size_of_us;      // Size in words of Update Sequence Number & Array
      ULONGLONG lsn;        // $LogFile Sequence Number
      WORD seq_no;          // Sequence number
      WORD hardlinks;       // Hard link count
      WORD offset_of_attr;  // Offset of the first Attribute
      Flag::FileRecord flags;  // Flags
      DWORD real_size;         // Real size of the FILE record
      DWORD alloc_size;        // Allocated size of the FILE record
      ULONGLONG ref_to_base;   // File reference to the base FILE record
      WORD next_attr_id;       // Next Attribute Id
      WORD align;              // Align to 4 byte boundary
      DWORD record_no;         // Number of this MFT Record
    };
    BYTE raw[1024];
  };

  WORD us_number;
  std::vector<WORD> us_array;
  size_t sector_size;

  FileRecordHeader(BYTE* buffer, size_t fileRecordSize, size_t sector_size)
  {
    this->sector_size = sector_size;
    _ASSERT(1024 == fileRecordSize);
    memcpy(&raw[0], buffer, fileRecordSize);

    if (magic == kFileRecordMagic)
    {
      us_array.reserve(fileRecordSize / sector_size);
      const gsl::not_null<WORD*> usnaddr =
          reinterpret_cast<WORD*>(buffer + offset_of_us);
      us_number = *usnaddr;
      const gsl::not_null<WORD*> usarray = usnaddr.get() + 1;

      for (WORD i = 0; i < fileRecordSize / sector_size; i++)
        us_array.push_back(usarray.get()[i]);
    }
    else
    {
      us_number = 0;
    }
  }

  // Verify US and update sectors
  bool PatchUS() noexcept
  {
    gsl::not_null<WORD*> sector = reinterpret_cast<WORD*>(&raw[0]);
    for (WORD value : us_array)
    {
      sector = sector.get() + ((sector_size >> 1) - 1);
      // USN error
      if (*sector != us_number)
      {
        return false;
      }
      // Write back correct data
      *sector = value;
      sector = sector.get() + 1;
    }
    return true;
  }

  const AttrHeaderCommon& HeaderCommon() noexcept
  {
    return *reinterpret_cast<const AttrHeaderCommon*>(&raw[0] + offset_of_attr);
  }
};
}  // namespace NtfsBrowser