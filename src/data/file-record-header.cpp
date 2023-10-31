#include <ntfs-browser/data/file-record-header.h>

#include <ntfs-browser/strategy.h>

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
    // USN error. Ignore if already patched (FULL_CACHE)
    if (*sector != us_number && *sector != value)
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

template <Strategy S>
FileRecordHeaderImpl<S> FileRecordHeader::Factory(std::span<const BYTE> buffer,
                                                  size_t sector_size)
{
  return {buffer, sector_size};
}

FileRecordHeaderImpl<Strategy::NO_CACHE>::FileRecordHeaderImpl(
    std::span<const BYTE> buffer, size_t sector_size)
    : FileRecordHeader(buffer, sector_size), data_(buffer)
{
}

const FileRecordHeader::Data*
    FileRecordHeaderImpl<Strategy::NO_CACHE>::GetData() const
{
  return reinterpret_cast<const Data*>(data_.data());
}

FileRecordHeaderImpl<Strategy::FULL_CACHE>::FileRecordHeaderImpl(
    std::span<const BYTE> buffer, size_t sector_size)
    : FileRecordHeader(buffer, sector_size)
{
  memcpy(&data_.raw[0], buffer.data(), buffer.size());
}

const FileRecordHeader::Data*
    FileRecordHeaderImpl<Strategy::FULL_CACHE>::GetData() const
{
  return &data_;
}

template struct FileRecordHeaderImpl<Strategy::NO_CACHE>;
template struct FileRecordHeaderImpl<Strategy::FULL_CACHE>;

template
FileRecordHeaderImpl<Strategy::NO_CACHE>
    FileRecordHeader::Factory(std::span<const BYTE> buffer, size_t sector_size);
template
FileRecordHeaderImpl<Strategy::FULL_CACHE>
    FileRecordHeader::Factory(std::span<const BYTE> buffer, size_t sector_size);

}  // namespace NtfsBrowser
