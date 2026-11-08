#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// The $MFT of the fake volumes starts at cluster 1.
constexpr std::size_t mft_offset = FakeImage::fake_cluster_size;

// Where a file record header keeps the offset of its first attribute.
constexpr std::size_t first_attribute_field = 20;

// Fields of an attribute header: the total size, the non-resident flag, the
// length and offset of a resident value, the start VCN of a non-resident
// attribute.
constexpr std::size_t attr_total_size_field = 4;
constexpr std::size_t attr_non_resident_field = 8;
constexpr std::size_t attr_value_length_field = 16;
constexpr std::size_t attr_value_offset_field = 20;
constexpr std::size_t attr_start_vcn_field = 16;

// Fields of a non-resident attribute header: the last VCN, and three sizes.
constexpr std::size_t attr_id_field = 14;
constexpr std::size_t attr_last_vcn_field = 24;
constexpr std::size_t attr_allocated_size_field = 40;
constexpr std::size_t attr_real_size_field = 48;
constexpr std::size_t attr_initialized_size_field = 56;

// Where the run list of a non-resident attribute is announced.
constexpr std::size_t attr_data_run_offset_field = 32;

// The fixed part of an $INDEX_ROOT value, which the index header follows, and
// the field of that header with the offset of the first entry. The offset is
// counted from the start of the index header.
constexpr std::size_t index_root_header_bytes = 16;
constexpr std::size_t index_root_entry_offset_field = 16;

// Fields of a $FILE_NAME value: the parent reference, and the name length.
constexpr std::size_t file_name_parent_field = 0;
constexpr std::size_t file_name_length_field = 64;

// A parent reference keeps the sequence number in its top 16 bits.
constexpr unsigned sequence_shift = 48;

// The run list of the $MFT that MapMft() writes: a header for a length of one
// byte and an offset of one byte, the cluster of the $MFT, then the end.
constexpr BYTE one_byte_run_header = 0x11;

// Fields of the record header: the number of bytes in use, and the offset and
// word count of the update sequence array. The end of the attributes takes
// the marker and four more bytes.
constexpr std::size_t used_field = 24;
constexpr std::size_t array_offset_field = 4;
constexpr std::size_t array_words_field = 6;
constexpr std::size_t end_bytes = 8;

// The sectors of a record are 512 bytes.
constexpr std::size_t sector_bytes = 512;

// End marker of the attributes of a record.
constexpr DWORD end_marker = 0xFFFFFFFF;

template <typename T>
T Load(const std::vector<BYTE>& image, std::size_t offset) {
  T value{};
  std::memcpy(&value, &image.at(offset), sizeof(T));
  return value;
}

template <typename T>
void Store(std::vector<BYTE>& image, std::size_t offset, T value) {
  // at() checks the last byte too, so a value at the end cannot overrun.
  static_cast<void>(image.at(offset + sizeof(T) - 1));
  std::memcpy(&image.at(offset), &value, sizeof(T));
}

// The bytes of record idx as the volume means them: the last word of each
// sector comes back from the update sequence array.
std::vector<BYTE> LogicalRecord(const std::vector<BYTE>& image, ULONGLONG idx) {
  const std::size_t offset = RecordEdit::RecordOffset(idx);
  static_cast<void>(image.at(offset + FakeImage::fake_file_record_size - 1));
  std::vector<BYTE> record(image.begin() + static_cast<std::ptrdiff_t>(offset),
                           image.begin() +
                               static_cast<std::ptrdiff_t>(
                                   offset + FakeImage::fake_file_record_size));
  const std::size_t array = Load<WORD>(record, array_offset_field);
  const std::size_t words = Load<WORD>(record, array_words_field);
  for (std::size_t sector = 0;
       sector + 1 < words && (sector + 1) * sector_bytes <= record.size();
       ++sector) {
    Store<WORD>(record, ((sector + 1) * sector_bytes) - sizeof(WORD),
                Load<WORD>(record, array + ((sector + 1) * sizeof(WORD))));
  }
  return record;
}

// Writes the logical bytes of a record back into the image: each sector ends
// with the update sequence number, and its own last word goes into the array.
void StoreLogicalRecord(std::vector<BYTE>& image, ULONGLONG idx,
                        std::vector<BYTE> record) {
  const std::size_t array = Load<WORD>(record, array_offset_field);
  const std::size_t words = Load<WORD>(record, array_words_field);
  const auto number = Load<WORD>(record, array);
  for (std::size_t sector = 0;
       sector + 1 < words && (sector + 1) * sector_bytes <= record.size();
       ++sector) {
    const std::size_t end = ((sector + 1) * sector_bytes) - sizeof(WORD);
    Store<WORD>(record, array + ((sector + 1) * sizeof(WORD)),
                Load<WORD>(record, end));
    Store<WORD>(record, end, number);
  }
  std::copy(record.begin(), record.end(),
            image.begin() +
                static_cast<std::ptrdiff_t>(RecordEdit::RecordOffset(idx)));
}

}  // namespace

std::size_t RecordEdit::ValueOffset(const std::vector<BYTE>& image,
                                    std::size_t attr) {
  return attr + Load<WORD>(image, attr + attr_value_offset_field);
}

std::size_t RecordEdit::RecordOffset(ULONGLONG idx) {
  return mft_offset +
         (static_cast<std::size_t>(idx) * FakeImage::fake_file_record_size);
}

std::size_t RecordEdit::Attribute(const std::vector<BYTE>& image, ULONGLONG idx,
                                  DWORD type, std::size_t nth) {
  const std::size_t record = RecordOffset(idx);
  const std::size_t record_end = record + FakeImage::fake_file_record_size;
  std::size_t offset =
      record + Load<WORD>(image, record + first_attribute_field);
  std::size_t seen = 0;
  while (offset + (2 * sizeof(DWORD)) <= record_end) {
    const auto found = Load<DWORD>(image, offset);
    const auto total_size = Load<DWORD>(image, offset + attr_total_size_field);
    if (found == end_marker || total_size == 0) {
      break;
    }
    if (found == type && seen++ == nth) {
      return offset;
    }
    offset += total_size;
  }
  throw std::out_of_range("the record has no such attribute");
}

void RecordEdit::BreakMagic(std::vector<BYTE>& image, ULONGLONG idx) {
  Store<DWORD>(image, RecordOffset(idx), 0);
}

void RecordEdit::SetTotalSize(std::vector<BYTE>& image, std::size_t attr,
                              DWORD total_size) {
  Store<DWORD>(image, attr + attr_total_size_field, total_size);
}

void RecordEdit::ShrinkLastAttribute(std::vector<BYTE>& image, std::size_t attr,
                                     DWORD total_size) {
  const auto old_end = Load<DWORD>(image, attr + attr_total_size_field);
  Store<DWORD>(image, attr + attr_total_size_field, total_size);
  for (std::size_t at = total_size; at < old_end + sizeof(DWORD); ++at) {
    Store<BYTE>(image, attr + at, 0);
  }
  Store<DWORD>(image, attr + total_size, end_marker);
}

void RecordEdit::SetNonResidentFlag(std::vector<BYTE>& image, std::size_t attr,
                                    BYTE flag) {
  Store<BYTE>(image, attr + attr_non_resident_field, flag);
}

void RecordEdit::SetType(std::vector<BYTE>& image, std::size_t attr,
                         DWORD type) {
  Store<DWORD>(image, attr, type);
}

void RecordEdit::SetStartVcn(std::vector<BYTE>& image, std::size_t attr,
                             ULONGLONG vcn) {
  Store<ULONGLONG>(image, attr + attr_start_vcn_field, vcn);
}

void RecordEdit::SetValueLength(std::vector<BYTE>& image, std::size_t attr,
                                DWORD length) {
  Store<DWORD>(image, attr + attr_value_length_field, length);
}

void RecordEdit::SetFileNameLength(std::vector<BYTE>& image, std::size_t attr,
                                   BYTE length) {
  Store<BYTE>(image,
              RecordEdit::ValueOffset(image, attr) + file_name_length_field,
              length);
}

void RecordEdit::SetFileNameParent(std::vector<BYTE>& image, std::size_t attr,
                                   ULONGLONG record, WORD sequence) {
  Store<ULONGLONG>(
      image, RecordEdit::ValueOffset(image, attr) + file_name_parent_field,
      record | (static_cast<ULONGLONG>(sequence) << sequence_shift));
}

std::size_t RecordEdit::FirstIndexRootEntry(const std::vector<BYTE>& image,
                                            std::size_t attr) {
  const std::size_t root = ValueOffset(image, attr);
  return root + index_root_header_bytes +
         Load<DWORD>(image, root + index_root_entry_offset_field);
}

void RecordEdit::SetRunList(std::vector<BYTE>& image, std::size_t attr,
                            const std::vector<BYTE>& runs) {
  const std::size_t start =
      attr + Load<WORD>(image, attr + attr_data_run_offset_field);
  for (std::size_t i = 0; i < runs.size(); ++i) {
    Store<BYTE>(image, start + i, runs[i]);
  }
}

void RecordEdit::ReplaceRunList(std::vector<BYTE>& image, std::size_t attr,
                                const std::vector<BYTE>& runs) {
  constexpr std::size_t alignment = 8;
  const std::size_t runs_offset =
      Load<WORD>(image, attr + attr_data_run_offset_field);
  const std::size_t old_total =
      Load<DWORD>(image, attr + attr_total_size_field);
  const std::size_t new_total =
      (runs_offset + runs.size() + alignment - 1) / alignment * alignment;
  // The record keeps the bytes of its last sector word for the fixup.
  const std::size_t record_end =
      (attr / FakeImage::fake_cluster_size) * FakeImage::fake_cluster_size +
      FakeImage::fake_file_record_size - 4;
  const std::size_t tail_start = attr + old_total;
  static_cast<void>(image.at(record_end));
  if (attr + new_total > record_end) {
    throw std::out_of_range("the run list does not fit in the record");
  }
  std::vector<BYTE> tail(
      image.begin() + static_cast<std::ptrdiff_t>(tail_start),
      image.begin() + static_cast<std::ptrdiff_t>(record_end));
  std::fill(image.begin() + static_cast<std::ptrdiff_t>(attr + runs_offset),
            image.begin() + static_cast<std::ptrdiff_t>(record_end), BYTE{0});
  for (std::size_t i = 0; i < runs.size(); ++i) {
    Store<BYTE>(image, attr + runs_offset + i, runs[i]);
  }
  Store<DWORD>(image, attr + attr_total_size_field,
               static_cast<DWORD>(new_total));
  const std::size_t room = record_end - (attr + new_total);
  for (std::size_t i = 0; i < std::min(tail.size(), room); ++i) {
    Store<BYTE>(image, attr + new_total + i, tail[i]);
  }
}

namespace {

// Bytes of the fixed header of a resident attribute, and of a non-resident
// one without the compressed size.
constexpr DWORD resident_header_bytes = 24;
constexpr DWORD non_resident_header_bytes = 64;

}  // namespace

// Offset where the next attribute of record idx goes: its end marker.
static std::size_t EndOfAttributes(const std::vector<BYTE>& image,
                                   std::size_t record) {
  std::size_t offset =
      record + Load<WORD>(image, record + first_attribute_field);
  while (Load<DWORD>(image, offset) != end_marker) {
    const auto total_size = Load<DWORD>(image, offset + attr_total_size_field);
    if (total_size == 0) {
      break;
    }
    offset += total_size;
  }
  return offset;
}

std::size_t RecordEdit::AppendResident(std::vector<BYTE>& image, ULONGLONG idx,
                                       DWORD type, WORD id,
                                       const std::vector<BYTE>& value) {
  const std::size_t record = RecordOffset(idx);
  const std::size_t attr = EndOfAttributes(image, record);
  const auto total = static_cast<DWORD>(resident_header_bytes + value.size());
  Store<DWORD>(image, attr, type);
  Store<DWORD>(image, attr + attr_total_size_field, total);
  Store<WORD>(image, attr + attr_id_field, id);
  Store<DWORD>(image, attr + attr_value_length_field,
               static_cast<DWORD>(value.size()));
  Store<WORD>(image, attr + attr_value_offset_field, resident_header_bytes);
  for (std::size_t i = 0; i < value.size(); ++i) {
    Store<BYTE>(image, attr + resident_header_bytes + i, value[i]);
  }
  Store<DWORD>(image, attr + total, end_marker);
  return attr;
}

std::size_t RecordEdit::AppendNonResident(std::vector<BYTE>& image,
                                          ULONGLONG idx, DWORD type,
                                          const std::vector<BYTE>& runs,
                                          ULONGLONG last_vcn,
                                          ULONGLONG allocated_size,
                                          ULONGLONG real_size) {
  const std::size_t record = RecordOffset(idx);
  const std::size_t attr = EndOfAttributes(image, record);
  const auto total =
      static_cast<DWORD>(non_resident_header_bytes + runs.size());
  Store<DWORD>(image, attr, type);
  Store<DWORD>(image, attr + attr_total_size_field, total);
  Store<BYTE>(image, attr + attr_non_resident_field, 1);
  Store<ULONGLONG>(image, attr + attr_last_vcn_field, last_vcn);
  Store<WORD>(image, attr + attr_data_run_offset_field,
              non_resident_header_bytes);
  Store<ULONGLONG>(image, attr + attr_allocated_size_field, allocated_size);
  Store<ULONGLONG>(image, attr + attr_real_size_field, real_size);
  Store<ULONGLONG>(image, attr + attr_initialized_size_field, real_size);
  for (std::size_t i = 0; i < runs.size(); ++i) {
    Store<BYTE>(image, attr + non_resident_header_bytes + i, runs[i]);
  }
  Store<DWORD>(image, attr + total, end_marker);
  return attr;
}

std::vector<std::vector<BYTE>>
    RecordEdit::Attributes(const std::vector<BYTE>& image, ULONGLONG idx) {
  const std::vector<BYTE> record = LogicalRecord(image, idx);
  std::vector<std::vector<BYTE>> attributes;
  std::size_t offset = Load<WORD>(record, first_attribute_field);
  while (Load<DWORD>(record, offset) != end_marker) {
    const auto total_size = Load<DWORD>(record, offset + attr_total_size_field);
    if (total_size == 0) {
      break;
    }
    static_cast<void>(record.at(offset + total_size - 1));
    attributes.emplace_back(
        record.begin() + static_cast<std::ptrdiff_t>(offset),
        record.begin() + static_cast<std::ptrdiff_t>(offset + total_size));
    offset += total_size;
  }
  return attributes;
}

void RecordEdit::SetAttributes(
    std::vector<BYTE>& image, ULONGLONG idx,
    const std::vector<std::vector<BYTE>>& attributes) {
  std::vector<BYTE> record = LogicalRecord(image, idx);
  const std::size_t first = Load<WORD>(record, first_attribute_field);
  const std::size_t limit = Load<WORD>(record, array_offset_field);
  std::fill(record.begin() + static_cast<std::ptrdiff_t>(first),
            record.begin() + static_cast<std::ptrdiff_t>(limit), BYTE{0});
  std::size_t at = first;
  for (const std::vector<BYTE>& attribute : attributes) {
    if (at + attribute.size() + (2 * sizeof(DWORD)) > limit) {
      throw std::out_of_range("the attributes do not fit in the record");
    }
    std::copy(attribute.begin(), attribute.end(),
              record.begin() + static_cast<std::ptrdiff_t>(at));
    at += attribute.size();
  }
  Store<DWORD>(record, at, end_marker);
  Store<DWORD>(record, used_field, static_cast<DWORD>(at + end_bytes));
  StoreLogicalRecord(image, idx, record);
}

void RecordEdit::MapMft(std::vector<BYTE>& image, ULONGLONG clusters,
                        BYTE first, ULONGLONG cluster_bytes) {
  const std::size_t attr = Attribute(image, 0, type_data);
  const ULONGLONG bytes = clusters * cluster_bytes;
  Store<ULONGLONG>(image, attr + attr_last_vcn_field, clusters - 1);
  Store<ULONGLONG>(image, attr + attr_allocated_size_field, bytes);
  Store<ULONGLONG>(image, attr + attr_real_size_field, bytes);
  Store<ULONGLONG>(image, attr + attr_initialized_size_field, bytes);
  if (clusters == 0 || clusters > std::numeric_limits<BYTE>::max()) {
    throw std::out_of_range("the run length takes one byte");
  }
  const std::size_t runs =
      attr + Load<WORD>(image, attr + attr_data_run_offset_field);
  Store<BYTE>(image, runs, one_byte_run_header);
  Store<BYTE>(image, runs + 1, static_cast<BYTE>(clusters));
  Store<BYTE>(image, runs + 2, first);
  Store<BYTE>(image, runs + 3, 0);
}

void RecordEdit::ClearAttributes(std::vector<BYTE>& image, ULONGLONG idx) {
  const std::size_t record = RecordOffset(idx);
  const std::size_t first =
      record + Load<WORD>(image, record + first_attribute_field);
  const std::size_t slot = record + FakeImage::fake_file_record_size - 4;
  for (std::size_t at = first; at < slot; ++at) {
    Store<BYTE>(image, at, 0);
  }
}

void RecordEdit::CopyRecord(std::vector<BYTE>& image, ULONGLONG from,
                            ULONGLONG to) {
  const std::size_t source = RecordOffset(from);
  static_cast<void>(image.at(source + FakeImage::fake_file_record_size - 1));
  std::memcpy(&image.at(RecordOffset(to)), &image[source],
              FakeImage::fake_file_record_size);
}

void RecordEdit::CopyRecordFrom(std::vector<BYTE>& image,
                                const std::vector<BYTE>& donor, ULONGLONG from,
                                ULONGLONG to) {
  const std::size_t source = RecordOffset(from);
  static_cast<void>(donor.at(source + FakeImage::fake_file_record_size - 1));
  static_cast<void>(
      image.at(RecordOffset(to) + FakeImage::fake_file_record_size - 1));
  std::memcpy(&image[RecordOffset(to)], &donor[source],
              FakeImage::fake_file_record_size);
}

void RecordEdit::StoreByte(std::vector<BYTE>& image, std::size_t offset,
                           BYTE value) {
  Store<BYTE>(image, offset, value);
}

void RecordEdit::StoreWord(std::vector<BYTE>& image, std::size_t offset,
                           WORD value) {
  Store<WORD>(image, offset, value);
}

void RecordEdit::StoreDword(std::vector<BYTE>& image, std::size_t offset,
                            DWORD value) {
  Store<DWORD>(image, offset, value);
}

void RecordEdit::StoreQword(std::vector<BYTE>& image, std::size_t offset,
                            ULONGLONG value) {
  Store<ULONGLONG>(image, offset, value);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
