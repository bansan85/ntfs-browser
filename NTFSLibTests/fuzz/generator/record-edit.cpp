#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
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

// Fields of a $FILE_NAME value: the parent reference, and the name length.
constexpr std::size_t file_name_parent_field = 0;
constexpr std::size_t file_name_length_field = 64;

// A parent reference keeps the sequence number in its top 16 bits.
constexpr unsigned sequence_shift = 48;

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
