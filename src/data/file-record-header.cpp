#include "file-record-header.h"

namespace NtfsBrowser
{

FileRecordHeader::FileRecordHeader(std::span<const BYTE> buffer,
                                   size_t sector_size)
    : sector_size(sector_size)
{
  _ASSERT(1024 == buffer.size());

  const Data* data = reinterpret_cast<const Data*>(buffer.data());

  if (data->magic != kFileRecordMagic)
  {
    us_number = 0;
    return;
  }

  us_array.reserve(buffer.size() / sector_size);
  const gsl::not_null<const WORD*> usnaddr =
      reinterpret_cast<const WORD*>(buffer.data() + data->offset_of_us);
  us_number = *usnaddr;
  const gsl::not_null<const WORD*> usarray = usnaddr.get() + 1;

  for (size_t i = 0; i < buffer.size() / sector_size; i++)
  {
    us_array.push_back(usarray.get()[i]);
  }
}

bool FileRecordHeader::PatchUS() noexcept
{
  gsl::not_null<WORD*> sector =
      const_cast<WORD*>(reinterpret_cast<const WORD*>(&GetData()->raw[0]));
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
  return *reinterpret_cast<const AttrHeaderCommon*>(&GetData()->raw[0] +
                                                    GetData()->offset_of_attr);
}

std::unique_ptr<FileRecordHeader>
    FileRecordHeader::Factory(std::span<const BYTE> buffer, size_t sector_size,
                              FileReader::Strategy strategy)
{
  switch (strategy)
  {
    case FileReader::Strategy::NO_CACHE:
    {
      return std::make_unique<FileRecordHeaderHeavy>(buffer, sector_size);
    }
    case FileReader::Strategy::FULL_CACHE:
    {
      return std::make_unique<FileRecordHeaderLight>(buffer, sector_size);
    }
    default:
    {
      return {};
    }
  }
}

FileRecordHeaderLight::FileRecordHeaderLight(std::span<const BYTE> buffer,
                                             size_t sector_size)
    : FileRecordHeader(buffer, sector_size), data_(buffer)
{
}

const FileRecordHeader::Data* FileRecordHeaderLight::GetData() const
{
  return reinterpret_cast<const Data*>(data_.data());
}

FileRecordHeaderHeavy::FileRecordHeaderHeavy(std::span<const BYTE> buffer,
                                             size_t sector_size)
    : FileRecordHeader(buffer, sector_size)
{
  memcpy(&data_.raw[0], buffer.data(), buffer.size());
}

const FileRecordHeader::Data* FileRecordHeaderHeavy::GetData() const
{
  return &data_;
}

}  // namespace NtfsBrowser
