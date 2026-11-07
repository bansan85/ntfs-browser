#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/bpb-edit.h>
#include <ntfs-browser/tool/fuzzgen/legacy-geometry.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>

#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// The old sector: the size of a file record, and of a cluster.
constexpr WORD legacy_bytes_per_sector = 1024;
constexpr BYTE legacy_sectors_per_cluster = 1;

// Size of one word of an update sequence array.
constexpr std::size_t word_bytes = sizeof(WORD);

// Gives a structure of size bytes an update sequence array of one word per
// sector of the given size plus the sequence number, in its last bytes.
// Fixups need no value: the bytes are still zero.
struct ArrayLayout {
  WORD offset_of_us;
  WORD size_of_us;
};

ArrayLayout LayoutFor(std::size_t structure_bytes, std::size_t sector_bytes) {
  const std::size_t words = 1 + (structure_bytes / sector_bytes);
  return {.offset_of_us =
              static_cast<WORD>(structure_bytes - (word_bytes * words)),
          .size_of_us = static_cast<WORD>(words)};
}

// Rewrites the array fields of the header at offset (they sit at bytes 4 and
// 6 in a file record and in an index block alike), when they have the layout
// the current builders give a structure of structure_bytes.
void Relocate(std::vector<BYTE>& image, std::size_t offset,
              std::size_t structure_bytes) {
  const ArrayLayout current =
      LayoutFor(structure_bytes, FakeImage::fake_bytes_per_sector);
  const ArrayLayout legacy =
      LayoutFor(structure_bytes, legacy_bytes_per_sector);
  ArrayLayout found{};
  std::memcpy(&found, &image.at(offset + 4), sizeof(found));
  if (found.offset_of_us != current.offset_of_us ||
      found.size_of_us != current.size_of_us) {
    return;
  }
  std::memcpy(&image.at(offset + 4), &legacy, sizeof(legacy));
}

// Fields of an attribute header the packing looks at: the total size, the
// non-resident flag, the length and offset of a resident value, and the offset
// of the run list of a non-resident attribute.
constexpr std::size_t attr_total_size_field = 4;
constexpr std::size_t attr_non_resident_field = 8;
constexpr std::size_t attr_value_length_field = 16;
constexpr std::size_t attr_value_offset_field = 20;
constexpr std::size_t attr_data_run_offset_field = 32;

// A run header holds the byte count of the length field in its low nibble and
// that of the offset field in its high nibble.
constexpr BYTE run_field_mask = 0x0F;
constexpr unsigned run_field_shift = 4;

// The end marker of the attributes of a record.
constexpr DWORD end_marker = 0xFFFFFFFF;

// Offset of the first attribute in a record, from the start of the record.
constexpr std::size_t first_attribute_field = 20;

template <typename T>
T Load(const std::vector<BYTE>& image, std::size_t offset) {
  T value{};
  std::memcpy(&value, &image.at(offset), sizeof(T));
  return value;
}

// The old builders gave a resident attribute exactly the size of its header
// and its value. The builders of today round it up to 8 bytes. Cuts the last
// attribute of the record at record back to the old size.
void PackAt(std::vector<BYTE>& image, std::size_t record) {
  const std::size_t record_end = record + FakeImage::fake_file_record_size;
  std::size_t offset =
      record + Load<WORD>(image, record + first_attribute_field);
  std::size_t last = 0;
  while (offset + (2 * sizeof(DWORD)) <= record_end) {
    const auto type = Load<DWORD>(image, offset);
    const auto total_size = Load<DWORD>(image, offset + attr_total_size_field);
    if (type == end_marker) {
      break;
    }
    if (total_size == 0) {
      return;
    }
    last = offset;
    offset += total_size;
  }
  if (last == 0) {
    return;
  }
  const auto total_size = Load<DWORD>(image, last + attr_total_size_field);
  DWORD packed = 0;
  if (Load<BYTE>(image, last + attr_non_resident_field) == 0) {
    packed = Load<WORD>(image, last + attr_value_offset_field) +
             Load<DWORD>(image, last + attr_value_length_field);
  } else {
    // A non-resident attribute ends with its run list, and the list ends with
    // a zero byte.
    std::size_t at = Load<WORD>(image, last + attr_data_run_offset_field);
    while (last + at < record_end && Load<BYTE>(image, last + at) != 0) {
      const BYTE header = Load<BYTE>(image, last + at);
      at += 1 + (header & run_field_mask) + (header >> run_field_shift);
    }
    packed = static_cast<DWORD>(at + 1);
  }
  if (packed >= total_size || last + total_size + sizeof(DWORD) > record_end ||
      Load<DWORD>(image, last + total_size) != end_marker) {
    return;
  }
  RecordEdit::ShrinkLastAttribute(image, last, packed);
}

// The $Volume record, and the length of the 16-byte value of the early era.
constexpr ULONGLONG volume_record = 3;
constexpr DWORD early_volume_information_length = 16;

}  // namespace

void LegacyGeometry::PackLastAttribute(std::vector<BYTE>& image,
                                       ULONGLONG idx) {
  PackAt(image, RecordEdit::RecordOffset(idx));
}

void LegacyGeometry::Apply(std::vector<BYTE>& image, Era era) {
  BpbEdit::Apply(image, [](Data::NtfsBpb& bpb) {
    bpb.bytes_per_sector = legacy_bytes_per_sector;
    bpb.sectors_per_cluster = legacy_sectors_per_cluster;
  });
  constexpr std::size_t header_bytes = 8;
  if (era == Era::Late) {
    PackLastAttribute(image, volume_record);
  } else {
    RecordEdit::SetValueLength(
        image,
        RecordEdit::Attribute(image, volume_record,
                              RecordEdit::type_volume_information),
        early_volume_information_length);
  }
  for (std::size_t offset = FakeImage::fake_cluster_size;
       offset + header_bytes <= image.size();
       offset += FakeImage::fake_cluster_size) {
    DWORD magic = 0;
    std::memcpy(&magic, &image.at(offset), sizeof(magic));
    if (magic == Data::FileRecordHeader::file_record_magic) {
      Relocate(image, offset, FakeImage::fake_file_record_size);
    } else if (magic == Data::index_block_magic) {
      // The block size is not in the block. The size_of_us of the current
      // layout tells it.
      WORD size_of_us = 0;
      std::memcpy(&size_of_us, &image.at(offset + 6), sizeof(size_of_us));
      if (size_of_us > 1) {
        Relocate(image, offset,
                 static_cast<std::size_t>(size_of_us - 1) *
                     FakeImage::fake_bytes_per_sector);
      }
    }
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
