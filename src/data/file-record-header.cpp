#include "file-record-header.h"

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <span>
#include <stdexcept>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/strategy.h>

#include "../internal-export.h"
#include "../ntfs-common.h"

namespace NtfsBrowser
{

FileRecordHeader::FileRecordHeader(std::span<const BYTE> buffer)
    : buffer_size_(buffer.size())
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

  // A forged offset_of_us can place the array past the buffer's end.
  const size_t sectors =
      UpdateSequenceBlockCount(buffer.size(), data->size_of_us);
  if (data->offset_of_us + 2 * (1 + sectors) > buffer.size())
  {
    throw std::runtime_error(
        "Update Sequence Array does not fit within the file record "
        "buffer.");
  }
  // A wrong size_of_us cannot make the loop below read out of bounds.
  us_array.reserve(sectors);
  // offset_of_us is not checked for alignment, so read the words as bytes.
  // The array fit check above covers the number and every word after it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  std::memcpy(&us_number, &buffer[data->offset_of_us], sizeof(us_number));

  for (size_t i = 0; i < sectors; i++)
  {
    WORD value = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&value, &buffer[data->offset_of_us + (sizeof(WORD) * (1 + i))],
                sizeof(value));
    us_array.push_back(value);
  }
}

bool FileRecordHeader::PatchUS() noexcept
{
  const std::span<WORD> words(
      const_cast<WORD*>(reinterpret_cast<const WORD*>(&GetData()->raw[0])),
      buffer_size_ / sizeof(WORD));
  size_t pos = 0;
  for (WORD const value : us_array)
  {
    // The last word of each sector holds the USN.
    pos += (kUpdateSequenceStride / sizeof(WORD)) - 1;
    if (pos >= words.size())
    {
      return false;
    }
    // pos < words.size() was checked just above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    WORD& sector = words[pos];
    // USN error. Ignore if already patched (FULL_CACHE)
    if (sector != us_number && sector != value)
    {
      return false;
    }
    // Write back correct data
    sector = value;
    pos++;
  }
  return true;
}

const AttrHeaderCommon* FileRecordHeader::HeaderCommon() noexcept
{
  WORD const offset_of_attr = GetData()->offset_of_attr;
  if (offset_of_attr + sizeof(AttrHeaderCommon) >= buffer_size_)
  {
    LogWarn("Offset of attr must be within the file record buffer");
    return nullptr;
  }
  return reinterpret_cast<const AttrHeaderCommon*>(
      &GetData()->raw[offset_of_attr]);
}

template <Strategy S>
FileRecordHeaderImpl<S> FileRecordHeader::Factory(std::span<const BYTE> buffer)
{
  return FileRecordHeaderImpl<S>{buffer};
}

FileRecordHeaderImpl<Strategy::NO_CACHE>::FileRecordHeaderImpl(
    std::span<const BYTE> buffer)
    : FileRecordHeader(buffer), data_(buffer)
{
}

const FileRecordHeader::Data*
    FileRecordHeaderImpl<Strategy::NO_CACHE>::GetData() const
{
  return reinterpret_cast<const Data*>(data_.data());
}

FileRecordHeaderImpl<Strategy::FULL_CACHE>::FileRecordHeaderImpl(
    std::span<const BYTE> buffer)
    : FileRecordHeader(buffer)
{
  memcpy(&data_.raw[0], buffer.data(), buffer.size());
}

const FileRecordHeader::Data*
    FileRecordHeaderImpl<Strategy::FULL_CACHE>::GetData() const
{
  return &data_;
}

// Class-level NTFS_BROWSER_EXPORT_TESTS_ONLY (on FileRecordHeader) does not
// reach a member function template's own explicit instantiations: each needs
// the macro again here, or the unit tests cannot link against it on a shared
// build.
template NTFS_BROWSER_EXPORT_TESTS_ONLY FileRecordHeaderImpl<Strategy::NO_CACHE>
    FileRecordHeader::Factory(std::span<const BYTE> buffer);
template NTFS_BROWSER_EXPORT_TESTS_ONLY
    FileRecordHeaderImpl<Strategy::FULL_CACHE>
    FileRecordHeader::Factory(std::span<const BYTE> buffer);

}  // namespace NtfsBrowser
