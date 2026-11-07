#include <ntfs-browser/win-types.h>

#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/hex-bytes.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/legacy-root-record-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/file-record-header.h"
#include "data/header-resident.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;

// The root directory record and the $Volume record.
constexpr ULONGLONG root_record = 5;
constexpr ULONGLONG volume_record = 3;

// Attribute type codes of the hand-made attributes.
constexpr DWORD type_standard_information = 0x10;
constexpr DWORD type_attribute_list = 0x20;
constexpr DWORD type_volume_name = 0x60;
constexpr DWORD type_unknown = 0xC0;

// The record flag word of a file that is in use. The directory bit is off.
constexpr WORD flag_in_use = 0x01;

// An attribute body of eight zero bytes, and one of sixteen.
constexpr const char* eight_zero_bytes = "00 00 00 00 00 00 00 00";
constexpr const char* sixteen_zero_bytes =
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";

// A run header (one byte of length, one of offset) that the attribute cuts off
// before its two field bytes.
constexpr const char* run_header_without_fields = "11";

// The size of the area that the cut-off run would map.
constexpr ULONGLONG cut_run_allocated_size = 0x400;

// Size an attribute declares when it is shorter than its own fixed header, and
// a value size that reaches far past its record.
constexpr DWORD total_size_below_header = 0x11;
constexpr DWORD value_length_past_record = 0x64;

// The fixed header of a resident attribute, and an end marker cut to its first
// byte: the fuzzer left the other three at zero.
constexpr std::size_t resident_header_bytes = 24;
constexpr DWORD end_marker_first_byte = 0xFF;

// An update sequence array that starts in the last word of the record, and
// one that starts right after the record.
constexpr WORD offset_of_us_in_last_word = 0x3FE;
constexpr WORD record_size = 0x400;

// A $DATA with a name of six characters whose offset lies past the end of the
// attribute. The six characters of the sentinel sit there.
constexpr BYTE overlong_name_length = 6;
constexpr WORD name_offset_past_attribute = 0x64;

// One entry of an $ATTRIBUTE_LIST of 0x28 bytes (type $DATA, base record 5),
// followed by ten bytes that cut the next entry short.
constexpr const char* entry_then_partial_header =
    "80 00 00 00 28 00 00 00 00 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00";

// A $STANDARD_INFORMATION of NTFS 3: the four times, then the flags.
constexpr const char* standard_information_v3 =
    "08 07 06 05 04 03 02 01 18 17 16 15 14 13 12 11 28 27 26 25 24 23 22 21 "
    "38 37 36 35 34 33 32 31 20 40 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";

// A small $DATA body, and the name of a volume.
constexpr const char* resident_data_body = "DE AD BE EF";
constexpr const char* volume_name_text =
    "54 00 45 00 53 00 54 00 56 00 4F 00 4C 00";

Image Base() { return FakeImage::BuildFakeNtfsImage(); }

// Makes the root record a plain file instead of a directory.
void MakeRootAFile(Image& image) {
  RecordEdit::Edit<Data::FileRecordHeader>(
      image, RecordEdit::RecordOffset(root_record), [](auto& header) {
        header.flags = static_cast<Data::FileRecordFlag>(flag_in_use);
      });
}

const std::vector<LegacyFixtures::Damaged>& Inputs() {
  static const std::vector<LegacyFixtures::Damaged> inputs = {
      {.name = "0724c913e1b2f0607bb5cd3ebfacb596db4458e9",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::AppendNonResident(
                 image, root_record, RecordEdit::type_index_allocation,
                 HexBytes::Decode(run_header_without_fields), 0,
                 cut_run_allocated_size, cut_run_allocated_size);
           },
       .early = true},
      {.name = "8fc085f7649f977b0ab5f67b5b9da055eebc56dd",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::AppendResident(image, root_record,
                                        type_standard_information, 0,
                                        HexBytes::Decode(eight_zero_bytes));
           },
       .early = true},
      {.name = "resident_attr_body_exceeds_bounds",
       .base = Base,
       .damage =
           [](Image& image) {
             const std::size_t attr = RecordEdit::AppendResident(
                 image, root_record, RecordEdit::type_index_root, 0,
                 HexBytes::Decode(eight_zero_bytes));
             RecordEdit::SetValueLength(image, attr, value_length_past_record);
           },
       .early = true},
      {.name = "f2a2482f50a933eeea4d1a506651884827c0952d",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::AppendResident(image, root_record,
                                        RecordEdit::type_index_root, 0,
                                        HexBytes::Decode(sixteen_zero_bytes));
           },
       .early = true},
      {.name = "resident_attr_body_out_of_bounds",
       .base = Base,
       .damage =
           [](Image& image) {
             MakeRootAFile(image);
             const std::size_t attr = RecordEdit::AppendResident(
                 image, root_record, type_unknown, 0, {});
             RecordEdit::SetTotalSize(image, attr, total_size_below_header);
             RecordEdit::Edit<Data::HeaderResident>(
                 image, attr, [](auto& header) { header.attr_offset = 0; });
             RecordEdit::StoreDword(image, attr + resident_header_bytes,
                                    end_marker_first_byte);
           },
       .early = true},
      {.name = "attr_name_exceeds_total_size",
       .base = Base,
       .damage =
           [](Image& image) {
             MakeRootAFile(image);
             const std::size_t attr = RecordEdit::AppendResident(
                 image, root_record, RecordEdit::type_data, 0,
                 HexBytes::Decode("00 00 00 00"));
             RecordEdit::Edit<Data::HeaderResident>(
                 image, attr, [](auto& header) {
                   header.header.name_length = overlong_name_length;
                   header.header.name_offset = name_offset_past_attribute;
                 });
             const std::wstring_view sentinel =
                 FakeImage::attr_name_bounds_sentinel;
             for (std::size_t i = 0; i < sentinel.size(); ++i) {
               RecordEdit::StoreWord(
                   image, attr + name_offset_past_attribute + (2 * i),
                   static_cast<WORD>(sentinel[i]));
             }
           },
       .early = true},
      {.name = "attribute_list_short_read",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::AppendResident(
                 image, root_record, type_attribute_list, 0,
                 HexBytes::Decode(entry_then_partial_header));
           },
       .early = true},
      {.name = "volume_name_resident_present",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::AppendResident(image, volume_record, type_volume_name,
                                        1, HexBytes::Decode(volume_name_text));
           }},
      {.name = "compressed_index_allocation_encrypted",
       .base = Base,
       .damage =
           [](Image& image) {
             MakeRootAFile(image);
             RecordEdit::AppendResident(
                 image, root_record, type_standard_information, 0,
                 HexBytes::Decode(standard_information_v3));
             RecordEdit::AppendResident(image, root_record,
                                        RecordEdit::type_data, 0,
                                        HexBytes::Decode(resident_data_body));
           }},
      {.name = "usn_array_exceeds_record_buffer",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::FileRecordHeader>(
                 image, RecordEdit::RecordOffset(root_record),
                 [](auto& header) {
                   header.offset_of_us = offset_of_us_in_last_word;
                   header.offset_of_attr = 0;
                   header.flags = static_cast<Data::FileRecordFlag>(0);
                 });
           },
       .early = true},
      {.name = "invalid_offset_of_us",
       .base = Base,
       .damage =
           [](Image& image) {
             RecordEdit::Edit<Data::FileRecordHeader>(
                 image, RecordEdit::RecordOffset(root_record),
                 [](auto& header) {
                   header.offset_of_us = record_size;
                   header.size_of_us = 0;
                   header.offset_of_attr = 0;
                   header.flags = static_cast<Data::FileRecordFlag>(0);
                 });
           },
       .early = true},
  };
  return inputs;
}

}  // namespace

void LegacyRootRecordScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::RegisterDamaged(registry, Inputs());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
