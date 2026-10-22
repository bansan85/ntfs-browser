#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>

#include "data/filename.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Size of the header of an index entry: file reference, entry size, key size,
// flags.
constexpr size_t entry_header_size = 16;

// Byte of a fake record that holds the check word of its first sector.
constexpr size_t first_sector_check_word = 510;

// Where a file record header keeps the used size.
constexpr size_t used_size_offset = 24;

// Where the fixup array of a fake record starts.
constexpr size_t fixup_array_offset = fake_file_record_size - us_slot_size;

// Writes a T at offset of bytes.
template <typename T>
void Store(std::vector<BYTE>& bytes, size_t offset, T value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

// Where record idx starts in the image.
size_t RecordStart(ULONGLONG idx) {
  return (static_cast<size_t>(mft_lcn) * cluster_size) +
         (static_cast<size_t>(idx) * fake_file_record_size);
}

}  // namespace

std::vector<BYTE> EncodeSlackName(const FakeSlackName& name) {
  using NTFS_BROWSER_NAMESPACE::Data::Filename;

  const std::vector<WORD> units = ToUtf16(name.name);
  const size_t header = name.with_entry_header ? entry_header_size : 0;
  const size_t content = offsetof(Filename, name) + (units.size() * 2);
  std::vector<BYTE> bytes(AlignAttrSize(header + content));

  if (name.with_entry_header) {
    // A file reference, the entry size and the key size.
    Store<ULONGLONG>(bytes, 0, 0x0001000000000040ULL);
    Store<WORD>(bytes, 8, static_cast<WORD>(bytes.size()));
    Store<WORD>(bytes, 10, static_cast<WORD>(content));
  }
  const auto put = [&](size_t offset, auto value) {
    Store(bytes, header + offset, value);
  };
  put(offsetof(Filename, parent_ref),
      MakeFileReference(name.parent_record, name.parent_sequence));
  put(offsetof(Filename, create_time), name.times.at(0));
  put(offsetof(Filename, alter_time), name.times.at(1));
  put(offsetof(Filename, mft_time), name.times.at(2));
  put(offsetof(Filename, read_time), name.times.at(3));
  put(offsetof(Filename, flags), name.flags);
  put(offsetof(Filename, name_length), static_cast<BYTE>(units.size()));
  put(offsetof(Filename, name_space), name.name_space);
  for (size_t i = 0; i < units.size(); ++i) {
    put(offsetof(Filename, name) + (i * 2), units.at(i));
  }
  return bytes;
}

size_t RecordEndMarkerOffset(const std::vector<BYTE>& image, ULONGLONG idx) {
  const size_t start = RecordStart(idx);
  size_t offset = 0;
  std::memcpy(&offset, &image.at(start + 20), sizeof(WORD));
  while (offset + 8 <= fake_file_record_size) {
    DWORD type = 0;
    DWORD total_size = 0;
    std::memcpy(&type, &image.at(start + offset), sizeof(type));
    if (type == static_cast<DWORD>(Attr::Type::All)) {
      return offset;
    }
    std::memcpy(&total_size, &image.at(start + offset + 4), sizeof(total_size));
    if (total_size == 0) {
      break;
    }
    offset += total_size;
  }
  throw std::out_of_range("record has no end marker");
}

void PutRecordSlack(std::vector<BYTE>& image, ULONGLONG idx,
                    size_t offset_from_marker, std::span<const BYTE> bytes) {
  const size_t first = RecordEndMarkerOffset(image, idx) + offset_from_marker;
  const size_t last = first + bytes.size();
  if (last > fixup_array_offset) {
    throw std::out_of_range("slack bytes touch the fixup array");
  }
  // The check word of the first sector MUST stay zero, or the fixups of the
  // fake record fail.
  const size_t check_first = std::max(first, first_sector_check_word);
  const size_t check_last = std::min(last, first_sector_check_word + 2);
  if (check_first < check_last &&
      std::ranges::any_of(
          bytes.subspan(check_first - first, check_last - check_first),
          [](BYTE value) { return value != 0; })) {
    throw std::out_of_range("slack bytes change the sector check word");
  }
  std::ranges::copy(bytes, image.begin() + static_cast<std::ptrdiff_t>(
                                               RecordStart(idx) + first));
}

void SetRecordUsedSize(std::vector<BYTE>& image, ULONGLONG idx,
                       DWORD used_size) {
  Store<DWORD>(image, RecordStart(idx) + used_size_offset, used_size);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
