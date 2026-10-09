#include "record/header.h"

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <span>
#include <stdexcept>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/strategy.h>

#include "internal-export.h"
#include "ntfs-common.h"

namespace NtfsBrowser::Record {

Header::Header(std::span<const BYTE> buffer) : buffer_size(buffer.size()) {
  if (buffer.size() < Data::FileRecordHeader::min_file_record_header_size) {
    throw std::runtime_error(
        "Buffer size of Record::Header is smaller than the minimum file "
        "record header size.");
  }
  if (buffer.size() > Data::FileRecordHeader::max_file_record_size) {
    throw std::runtime_error(
        "Buffer size of Record::Header exceeds the maximum supported file "
        "record size.");
  }

  const auto* data =
      reinterpret_cast<const Data::FileRecordHeader*>(buffer.data());

  if (data->magic != Data::FileRecordHeader::file_record_magic) {
    us_number = 0;
    return;
  }

  if (data->offset_of_us >= buffer.size()) {
    throw std::runtime_error("Offset must be lower than 1024.");
  }

  // A forged offset_of_us can place the array past the buffer's end.
  const size_t sectors = Data::FileRecordHeader::UpdateSequenceBlockCount(
      buffer.size(), data->size_of_us);
  if (data->offset_of_us + 2 * (1 + sectors) > buffer.size()) {
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

  for (size_t i = 0; i < sectors; i++) {
    WORD value = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&value, &buffer[data->offset_of_us + (sizeof(WORD) * (1 + i))],
                sizeof(value));
    us_array.push_back(value);
  }
}

bool Header::PatchUS() noexcept {
  // The update sequence is patched in place, in the buffer the caller owns.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
  const std::span<WORD> words(
      const_cast<WORD*>(reinterpret_cast<const WORD*>(&GetData()->raw[0])),
      buffer_size / sizeof(WORD));
  // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
  size_t pos = 0;
  for (WORD const value : us_array) {
    // The last word of each sector holds the USN.
    pos += (Data::FileRecordHeader::update_sequence_stride / sizeof(WORD)) - 1;
    if (pos >= words.size()) {
      return false;
    }
    // pos < words.size() was checked just above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    WORD& sector = words[pos];
    // USN error. Ignore if already patched (FullCache)
    if (sector != us_number && sector != value) {
      return false;
    }
    // Write back correct data
    sector = value;
    pos++;
  }
  return true;
}

const AttrHeaderCommon* Header::HeaderCommon() const noexcept {
  WORD const offset_of_attr = GetData()->offset_of_attr;
  if (offset_of_attr + sizeof(AttrHeaderCommon) >= buffer_size) {
    Log::Warn("Offset of attr must be within the file record buffer");
    return nullptr;
  }
  return reinterpret_cast<const AttrHeaderCommon*>(
      &GetData()->raw[offset_of_attr]);
}

HeaderImpl<Strategy::NoCache>::HeaderImpl(std::span<const BYTE> buffer)
    : Header(buffer), data(buffer) {}

const Data::FileRecordHeader* HeaderImpl<Strategy::NoCache>::GetData() const {
  return reinterpret_cast<const Data::FileRecordHeader*>(data.data());
}

HeaderImpl<Strategy::FullCache>::HeaderImpl(std::span<const BYTE> buffer)
    : Header(buffer) {
  memcpy(&data.raw[0], buffer.data(), buffer.size());
}

const Data::FileRecordHeader* HeaderImpl<Strategy::FullCache>::GetData() const {
  return &data;
}

}  // namespace NtfsBrowser::Record
