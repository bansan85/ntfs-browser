#include "file-record-header.h"

namespace NtfsBrowser
{

FileRecordHeader::FileRecordHeader(std::span<const BYTE> buffer,
                                   size_t sector_size)
    : sector_size(sector_size)
{
  _ASSERT(1024 == buffer.size());
  memcpy(&raw[0], buffer.data(), buffer.size());

  if (magic != kFileRecordMagic)
  {
    us_number = 0;
    return;
  }

  us_array.reserve(buffer.size() / sector_size);
  const gsl::not_null<const WORD*> usnaddr =
      reinterpret_cast<const WORD*>(buffer.data() + offset_of_us);
  us_number = *usnaddr;
  const gsl::not_null<const WORD*> usarray = usnaddr.get() + 1;

  for (size_t i = 0; i < buffer.size() / sector_size; i++)
  {
    us_array.push_back(usarray.get()[i]);
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
