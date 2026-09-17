#include <ntfs-browser/data/file-record-header.h>

#include <ntfs-browser/strategy.h>
#include "../ntfs-common.h"
#include <ntfs-browser/data/attr-header-common.h>

#include <cstring>
#include <stdexcept>

namespace NtfsBrowser
{

FileRecordHeader::FileRecordHeader(std::span<const BYTE> buffer,
                                   size_t sector_size)
    : sector_size(sector_size), buffer_size_(buffer.size())
{
  if (buffer.size() < kMinFileRecordHeaderSize)
  {
    throw std::runtime_error(
        "Buffer size of FileRecordHeader is smaller than the minimum file "
        "record header size.");
  }
  if (buffer.size() > kMaxFileRecordSize)
  {
    throw std::runtime_error(
        "Buffer size of FileRecordHeader exceeds the maximum supported file "
        "record size.");
  }

  const Data* data = reinterpret_cast<const Data*>(buffer.data());

  if (data->magic != kFileRecordMagic)
  {
    us_number = 0;
    return;
  }

  if (data->offset_of_us >= buffer.size())
  {
    throw std::runtime_error("Offset must be lower than 1024.");
  }

  // A small sector_size can place the array past the buffer's end.
  const size_t sectors = buffer.size() / sector_size;
  if (data->offset_of_us + 2 * (1 + sectors) > buffer.size())
  {
    throw std::runtime_error(
        "Update Sequence Array does not fit within the file record "
        "buffer.");
  }
  // A wrong size_of_us cannot make the loop below read out of bounds.
  us_array.reserve(sectors);
  const gsl::not_null<const WORD*> usnaddr =
      reinterpret_cast<const WORD*>(buffer.data() + data->offset_of_us);
  us_number = *usnaddr;
  const gsl::not_null<const WORD*> usarray = usnaddr.get() + 1;

  for (size_t i = 0; i < sectors; i++)
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

const AttrHeaderCommon* FileRecordHeader::HeaderCommon() noexcept
{
  WORD offset_of_attr = GetData()->offset_of_attr;
  if (offset_of_attr + sizeof(AttrHeaderCommon) >= buffer_size_)
  {
    NTFS_TRACE("Offset of attr must be within the file record buffer\n");
    return nullptr;
  }
  return reinterpret_cast<const AttrHeaderCommon*>(&GetData()->raw[0] +
                                                   offset_of_attr);
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

template FileRecordHeaderImpl<Strategy::NO_CACHE>
    FileRecordHeader::Factory(std::span<const BYTE> buffer, size_t sector_size);
template FileRecordHeaderImpl<Strategy::FULL_CACHE>
    FileRecordHeader::Factory(std::span<const BYTE> buffer, size_t sector_size);

}  // namespace NtfsBrowser
