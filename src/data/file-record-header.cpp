#include "file-record-header.h"

namespace NtfsBrowser
{

FileRecordHeader::FileRecordHeader(BYTE* buffer, size_t fileRecordSize,
                                   size_t sector_size)
    : sector_size(sector_size)
{
  _ASSERT(1024 == fileRecordSize);
  memcpy(&raw[0], buffer, fileRecordSize);

  if (magic == kFileRecordMagic)
  {
    us_array.reserve(fileRecordSize / sector_size);
    const gsl::not_null<WORD*> usnaddr =
        reinterpret_cast<WORD*>(buffer + offset_of_us);
    us_number = *usnaddr;
    const gsl::not_null<WORD*> usarray = usnaddr.get() + 1;

    for (size_t i = 0; i < fileRecordSize / sector_size; i++)
    {
      us_array.push_back(usarray.get()[i]);
    }
  }
  else
  {
    us_number = 0;
  }
}

bool FileRecordHeader::PatchUS() noexcept
{
  gsl::not_null<WORD*> sector = reinterpret_cast<WORD*>(&raw[0]);
  for (WORD value : us_array)
  {
    sector = sector.get() + ((sector_size >> 1U) - 1);
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

const AttrHeaderCommon& FileRecordHeader::HeaderCommon() noexcept
{
  return *reinterpret_cast<const AttrHeaderCommon*>(&raw[0] + offset_of_attr);
}
}  // namespace NtfsBrowser
