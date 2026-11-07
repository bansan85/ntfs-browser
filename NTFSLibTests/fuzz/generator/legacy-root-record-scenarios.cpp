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
#include "data/header-non-resident.h"
#include "data/header-resident.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;

// The record of the $MFT, the root directory record and the $Volume record.
constexpr ULONGLONG mft_record = 0;
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

// A $VOLUME_INFORMATION value of eight bytes, four short of the twelve that the
// attribute needs, and the size of the attribute that holds it.
constexpr DWORD short_volume_information_length = 8;
constexpr DWORD short_volume_information_total_size = 32;

// Makes the root record a plain file instead of a directory.
void MakeRootAFile(Image& image) {
  RecordEdit::Edit<Data::FileRecordHeader>(
      image, RecordEdit::RecordOffset(root_record), [](auto& header) {
        header.flags = static_cast<Data::FileRecordFlag>(flag_in_use);
      });
}

// The type code of the $EFS stream, which is a $LOGGED_UTILITY_STREAM named
// "$EFS", and the attribute flags: encrypted, and encrypted with compressed.
constexpr DWORD type_logged_utility_stream = 0x100;
constexpr WORD attr_flag_encrypted = 0x4000;
constexpr WORD attr_flag_encrypted_and_compressed = 0x4001;

// The name of the stream, in UTF-16, and its length in characters.
constexpr const char* efs_name_units = "24 00 45 00 46 00 53 00";
constexpr BYTE efs_name_length = 4;

// Where a non-resident attribute keeps its name: after the 64 bytes of the
// header. The run list follows the name. A resident one keeps the name after
// its 24 bytes, and the value after the name.
constexpr WORD non_resident_name_offset = 0x40;
constexpr WORD non_resident_runs_offset = 0x48;
constexpr WORD resident_name_offset = 0x18;
constexpr WORD resident_value_offset = 0x20;

// An empty run list, a stream of 131072 bytes (more than the library reads)
// and one of 100 bytes whose run list holds no run (so no byte can be read).
constexpr ULONGLONG efs_size_too_large = 0x20000;
constexpr ULONGLONG efs_size_unreadable = 100;
constexpr const char* empty_run_list = "00";

// A resident $EFS body of four zero bytes, too short to be a stream.
constexpr const char* malformed_efs_body = "00 00 00 00";

Image Base() { return FakeImage::BuildFakeNtfsImage(); }

// The $MFT of the inputs about one file is a single record: a stream of one
// cluster whose run list is empty, in an attribute of the size of its header
// and that list.
constexpr ULONGLONG single_record_mft_size = 0x400;
constexpr DWORD single_record_mft_attribute_size = 0x41;

// Gives the $MFT the form of a volume that holds one file only.
void ShrinkMft(Image& image) {
  const std::size_t data =
      RecordEdit::Attribute(image, mft_record, RecordEdit::type_data);
  RecordEdit::Edit<Data::HeaderNonResident>(image, data, [](auto& header) {
    header.alloc_size = single_record_mft_size;
    header.real_size = single_record_mft_size;
    header.ini_size = single_record_mft_size;
  });
  RecordEdit::ShrinkLastAttribute(image, data,
                                  single_record_mft_attribute_size);
}

// Puts the root in the state of a file with no other attribute than a
// non-resident $EFS stream of size bytes with no run, then an encrypted
// non-resident $DATA with no run.
void PutEfsStream(Image& image, ULONGLONG size) {
  ShrinkMft(image);
  MakeRootAFile(image);
  RecordEdit::ClearAttributes(image, root_record);
  std::vector<BYTE> name_then_runs = HexBytes::Decode(efs_name_units);
  const std::vector<BYTE> runs = HexBytes::Decode(empty_run_list);
  name_then_runs.insert(name_then_runs.end(), runs.begin(), runs.end());
  const std::size_t efs = RecordEdit::AppendNonResident(
      image, root_record, type_logged_utility_stream, name_then_runs, 0, size,
      size);
  RecordEdit::Edit<Data::HeaderNonResident>(image, efs, [](auto& header) {
    header.header.name_length = efs_name_length;
    header.header.name_offset = non_resident_name_offset;
    header.data_run_offset = non_resident_runs_offset;
  });
  const std::size_t data = RecordEdit::AppendNonResident(
      image, root_record, RecordEdit::type_data, runs, 0, 0, 0);
  RecordEdit::Edit<Data::HeaderNonResident>(image, data, [](auto& header) {
    header.header.flags = attr_flag_encrypted;
  });
}

// Puts a resident $EFS of four zero bytes, then an encrypted non-resident
// $DATA with no run.
void PutMalformedEfs(Image& image) {
  ShrinkMft(image);
  MakeRootAFile(image);
  RecordEdit::ClearAttributes(image, root_record);
  std::vector<BYTE> name_then_body = HexBytes::Decode(efs_name_units);
  const std::vector<BYTE> body = HexBytes::Decode(malformed_efs_body);
  name_then_body.insert(name_then_body.end(), body.begin(), body.end());
  const std::size_t efs = RecordEdit::AppendResident(
      image, root_record, type_logged_utility_stream, 0, name_then_body);
  RecordEdit::Edit<Data::HeaderResident>(image, efs, [&](auto& header) {
    header.header.name_length = efs_name_length;
    header.header.name_offset = resident_name_offset;
    header.attr_offset = resident_value_offset;
    header.attr_size = static_cast<DWORD>(body.size());
  });
  const std::size_t data =
      RecordEdit::AppendNonResident(image, root_record, RecordEdit::type_data,
                                    HexBytes::Decode(empty_run_list), 0, 0, 0);
  RecordEdit::Edit<Data::HeaderNonResident>(image, data, [](auto& header) {
    header.header.flags = attr_flag_encrypted;
  });
}

// Puts a resident $DATA of four zero bytes, with flags, as the only attribute
// of the root file.
void PutFlaggedData(Image& image, WORD flags) {
  ShrinkMft(image);
  MakeRootAFile(image);
  RecordEdit::ClearAttributes(image, root_record);
  const std::size_t data =
      RecordEdit::AppendResident(image, root_record, RecordEdit::type_data, 0,
                                 HexBytes::Decode("00 00 00 00"));
  RecordEdit::Edit<Data::HeaderResident>(
      image, data, [&](auto& header) { header.header.flags = flags; });
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
      {.name = "9d6b29a12783a8d0595bf861671e5401493570b5",
       .base = Base,
       .damage =
           [](Image& image) {
             const std::size_t attr = RecordEdit::Attribute(
                 image, volume_record, RecordEdit::type_volume_information);
             RecordEdit::SetValueLength(image, attr,
                                        short_volume_information_length);
             RecordEdit::ShrinkLastAttribute(
                 image, attr, short_volume_information_total_size);
           }},
      {.name = "efs_stream_too_large",
       .base = Base,
       .damage = [](Image& image) { PutEfsStream(image, efs_size_too_large); },
       .needs = Feature::Efs},
      {.name = "efs_stream_read_failure",
       .base = Base,
       .damage = [](Image& image) { PutEfsStream(image, efs_size_unreadable); },
       .needs = Feature::Efs},
      {.name = "efs_stream_malformed",
       .base = Base,
       .damage = PutMalformedEfs,
       .needs = Feature::Efs},
      {.name = "resident_data_flagged_encrypted",
       .base = Base,
       .damage =
           [](Image& image) { PutFlaggedData(image, attr_flag_encrypted); }},
      {.name = "data_flagged_compressed_and_encrypted",
       .base = Base,
       .damage =
           [](Image& image) {
             PutFlaggedData(image, attr_flag_encrypted_and_compressed);
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
