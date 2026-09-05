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
  // Range check (bug F9): a file record can never be smaller than
  // kMinFileRecordHeaderSize (too small to even hold Data's named header
  // fields), and FileRecordHeaderImpl<FULL_CACHE>'s ctor memcpy()s the whole
  // buffer into a by-value Data data_, so it can never be larger than
  // Data::raw's own capacity (kMaxFileRecordSize) either - that would
  // overflow data_. This used to be a hard `buffer.size() == 1024` check,
  // which incidentally rejected every 4Kn (4096-byte-sector) NTFS volume:
  // such a volume necessarily has 4096-byte file records (a record can never
  // be smaller than a sector), a perfectly legal size this range now
  // accepts. See F9 in docs/bug-reports/2026-09-03-full-repo.md.
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

  // The Update Sequence Array holds one WORD per sector in the record, plus
  // the leading USN itself (data->offset_of_us >= buffer.size() above only
  // guards that first WORD - it says nothing about the array that follows).
  // Without this check, a small sector_size (only guarded elsewhere to be
  // >= sizeof(WORD)) combined with an offset_of_us near the end of buffer
  // makes the loop below read far past buffer's declared end.
  const size_t sectors = buffer.size() / sector_size;
  if (data->offset_of_us + 2 * (1 + sectors) > buffer.size())
  {
    throw std::runtime_error(
        "Update Sequence Array does not fit within the file record "
        "buffer.");
  }
  // size_of_us is the on-disk field that is supposed to state the array's
  // real size (USN + one WORD per sector), but it is never used to size
  // anything below - sectors (derived straight from buffer.size() and
  // sector_size, both already validated) is what actually bounds the loop,
  // so the read stays safe regardless of what size_of_us claims. It is
  // deliberately not cross-checked against sectors + 1 here: unlike
  // offset_of_us, disagreeing with it can't cause an out-of-bounds read.

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
  // Bound against this instance's own buffer_size_ (bug F9), not against
  // sizeof(Data::raw) - Data::raw's capacity now has to cover the largest
  // supported record size (kMaxFileRecordSize, 4096), but a given instance's
  // real record can be smaller (eg. the common 1024-byte case). Bounding
  // against the union's static capacity instead of the actual buffer would
  // let offset_of_attr point past this instance's real, exactly-buffer_size_
  // -long backing storage - a heap over-read under Strategy::NO_CACHE (whose
  // data_ is a std::span over the real allocation), or a read of
  // uninitialized bytes under Strategy::FULL_CACHE (whose data_ only has
  // buffer_size_ bytes memcpy'd in).
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
