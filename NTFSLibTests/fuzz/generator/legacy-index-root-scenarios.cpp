#include "legacy-index-root-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/file-record-header.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "hex-bytes.h"
#include "legacy-fixtures.h"
#include "record-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;

// The $MFT record and the root directory record.
constexpr ULONGLONG mft_record = 0;
constexpr ULONGLONG root_record = 5;

// A "FILE" magic that was overwritten.
constexpr DWORD overwritten_magic = 0x58585858;

// An update sequence array offset past the end of the 1 KiB record.
constexpr WORD offset_of_us_past_record = 0x800;

// An update sequence array that ends 12 bytes before the record does, holding
// the sequence number 1, with a different word at the end of each sector.
constexpr WORD moved_offset_of_us = 0x3F0;
constexpr WORD moved_sequence_number = 1;
constexpr WORD other_sector_end = 0x1234;
constexpr std::size_t first_sector_end = 0x1FE;
constexpr std::size_t last_sector_end = 0x3FE;

// An index entry whose size cannot hold its own header, and one that cannot
// hold the name it declares.
constexpr WORD entry_size_below_header = 0x10;
constexpr WORD entry_size_below_name = 0x30;

// An entry offset that points past the end of the attribute.
constexpr DWORD entry_offset_past_attribute = 0x78;

// A run list with one run of a single cluster, whose offset byte 0xF0 is
// negative: the second run read from a list that has none yet.
constexpr const char* negative_offset_run = "11 01 F0";

// The root directory with its own $INDEX_ROOT: one entry named "Foo".
Image Base() { return FakeImage::BuildFakeNtfsImageWithRootIndexRootEntry(); }

std::size_t IndexRoot(const Image& image) {
  return RecordEdit::Attribute(image, root_record, RecordEdit::type_index_root);
}

// Types of the attributes that follow the index: a $BITMAP, and an
// $OBJECT_ID that the reader does not handle.
constexpr DWORD type_bitmap = 0xB0;
constexpr DWORD type_object_id = 0x40;

// The id the second attribute of the root takes, and a body of zero bytes.
constexpr WORD second_attribute_id = 7;
constexpr const char* eight_zero_bytes = "00 00 00 00 00 00 00 00";
constexpr const char* sixteen_zero_bytes =
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";

// The fixed header of a resident attribute, and the bytes the used size of a
// record counts after the end marker.
constexpr DWORD resident_header_bytes = 24;
constexpr DWORD used_size_tail = 8;

// Puts a second attribute after the index of the root, and counts it in the
// used size of the record.
void AppendAfterIndex(Image& image, DWORD type, const char* body) {
  const std::size_t attr = RecordEdit::AppendResident(
      image, root_record, type, second_attribute_id, HexBytes::Decode(body));
  const std::size_t record = RecordEdit::RecordOffset(root_record);
  RecordEdit::Edit<Data::FileRecordHeader>(image, record, [&](auto& header) {
    header.real_size =
        static_cast<DWORD>(attr - record + HexBytes::Decode(body).size() +
                           resident_header_bytes + used_size_tail);
  });
}

const std::vector<LegacyFixtures::Damaged>& Inputs() {
  static const std::vector<LegacyFixtures::Damaged> inputs = {
      {.name = "boot_sector_read_failure",
       .base = Base,
       .damage = [](Image&) {}},
      {.name = "file_reader_read_failure",
       .base = Base,
       .damage = [](Image&) {}},
      {.name = "index_entry_no_filename_stream",
       .base = Base,
       .damage = [](Image&) {}},
      {.name = "index_root_real_entry", .base = Base, .damage = [](Image&) {}},
      {.name = "traverse_attrs_empty_callback",
       .base = Base,
       .damage = [](Image&) {}},
      {.name = "file_record_invalid_magic",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::FileRecordHeader>(
                 image, RecordEdit::RecordOffset(root_record),
                 [](auto& header) { header.magic = overwritten_magic; });
           }},
      {.name = "file_record_read_failure",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::FileRecordHeader>(
                 image, RecordEdit::RecordOffset(root_record),
                 [](auto& header) {
                   header.offset_of_us = offset_of_us_past_record;
                 });
           }},
      {.name = "file_record_usn_mismatch",
       .base = Base,
       .damage =
           [](Image& image) {
             const std::size_t record = RecordEdit::RecordOffset(root_record);
             RecordEdit::Edit<Data::FileRecordHeader>(
                 image, record, [](auto& header) {
                   header.offset_of_us = moved_offset_of_us;
                 });
             RecordEdit::StoreWord(image, record + moved_offset_of_us,
                                   moved_sequence_number);
             RecordEdit::StoreWord(image, record + first_sector_end,
                                   other_sector_end);
             RecordEdit::StoreWord(image, record + last_sector_end,
                                   other_sector_end);
           }},
      {.name = "index_entry_stream_exceeds_bounds",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::IndexEntry>(
                 image,
                 RecordEdit::FirstIndexRootEntry(image, IndexRoot(image)),
                 [](auto& entry) { entry.size = entry_size_below_header; });
           }},
      {.name = "index_entry_stream_smaller_than_expected",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::IndexEntry>(
                 image,
                 RecordEdit::FirstIndexRootEntry(image, IndexRoot(image)),
                 [](auto& entry) { entry.size = entry_size_below_name; });
           }},
      {.name = "index_root_entry_header_exceeds_bounds",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::IndexRoot>(
                 image, RecordEdit::ValueOffset(image, IndexRoot(image)),
                 [](auto& root) {
                   root.entry_offset = entry_offset_past_attribute;
                 });
           }},
      {.name = "index_root_view_not_supported",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::IndexRoot>(
                 image, RecordEdit::ValueOffset(image, IndexRoot(image)),
                 [](auto& root) {
                   root.attr_type = static_cast<Attr::Type>(0);
                 });
           }},
      {.name = "data_run_decode_error_second",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::SetRunList(
                 image,
                 RecordEdit::Attribute(image, mft_record,
                                       RecordEdit::type_data),
                 HexBytes::Decode(negative_offset_run));
           }},
      {.name = "bitmap_resident_data_read",
       .base = Base,
       .damage =
           [](Image& image) {
             AppendAfterIndex(image, type_bitmap, eight_zero_bytes);
           }},
      {.name = "file_record_unhandled_attribute",
       .base = Base,
       .damage =
           [](Image& image) {
             AppendAfterIndex(image, type_object_id, sixteen_zero_bytes);
           }},
  };
  return inputs;
}

}  // namespace

void LegacyIndexRootScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::RegisterDamaged(registry, Inputs());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
