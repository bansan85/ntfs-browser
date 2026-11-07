#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/bpb-edit.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/legacy-residency-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;
using Probe = RecordedStream::Probe;

// The $Volume record and the root directory record.
constexpr ULONGLONG volume_record = 3;
constexpr ULONGLONG root_record = 5;

// Where a record keeps its first attribute.
constexpr std::size_t first_attribute_offset = 0x30;

// Attribute types of the hand-made attributes.
constexpr DWORD type_standard_information = 0x10;
constexpr DWORD type_volume_name = 0x60;

// The size of the header of a non-resident attribute without a run list, and
// the size of one of a resident attribute without a value.
constexpr DWORD non_resident_header_bytes = 0x40;
constexpr DWORD resident_header_bytes = 0x18;

// The flags of a directory that is in use, and of one that is not.
constexpr Data::FileRecordFlag directory_in_use =
    Data::FileRecordFlag::InUse | Data::FileRecordFlag::Dir;
constexpr Data::FileRecordFlag directory_deleted = Data::FileRecordFlag::Dir;

// An index block size of 2^17 bytes, in the negative-exponent form of the
// boot sector: more than a block of the full cache, which has 64 KiB.
constexpr DWORD shift_17_index_block = 0xEF;

Image Base() { return FakeImage::BuildFakeNtfsImage(); }

// Gives record idx the given flags and no attribute at all.
void EmptyRecord(Image& image, ULONGLONG idx, Data::FileRecordFlag flags) {
  RecordEdit::ClearAttributes(image, idx);
  RecordEdit::Edit<Data::FileRecordHeader>(
      image, RecordEdit::RecordOffset(idx),
      [&](auto& header) { header.flags = flags; });
}

// Writes the header of one attribute at the start of the attributes of record
// idx, and nothing after it: the first builders left the other fields at zero
// and wrote no end marker.
void BareAttribute(Image& image, ULONGLONG idx, DWORD type, DWORD total_size,
                   BYTE non_resident) {
  RecordEdit::Edit<Attr::HeaderCommon>(
      image, RecordEdit::RecordOffset(idx) + first_attribute_offset,
      [&](auto& header) {
        header.type = static_cast<Attr::Type>(type);
        header.total_size = total_size;
        header.non_resident = non_resident;
      });
}

// The root is a directory in use and holds one bare attribute.
LegacyFixtures::Damaged RootAttribute(const char* name, DWORD type,
                                      DWORD total_size, BYTE non_resident) {
  return {.name = name,
          .base = Base,
          .damage =
              [=](Image& image) {
                EmptyRecord(image, root_record, directory_in_use);
                BareAttribute(image, root_record, type, total_size,
                              non_resident);
              },
          .probe = Probe::FullCache};
}

// The root is a directory in use without attribute, and the $Volume record
// holds one bare non-resident attribute in place of its own.
LegacyFixtures::Damaged VolumeAttribute(const char* name, DWORD type) {
  return {.name = name,
          .base = Base,
          .damage =
              [=](Image& image) {
                EmptyRecord(image, root_record, directory_in_use);
                RecordEdit::ClearAttributes(image, volume_record);
                BareAttribute(image, volume_record, type,
                              non_resident_header_bytes, 1);
              },
          .probe = Probe::FullCache};
}

const std::vector<LegacyFixtures::Damaged>& Inputs() {
  static const std::vector<LegacyFixtures::Damaged> inputs = {
      {.name = "root_record_deleted_skips_parse_attrs",
       .base = Base,
       .damage =
           [](Image& image) {
             EmptyRecord(image, root_record, directory_deleted);
           },
       .probe = Probe::FullCache},
      RootAttribute("index_allocation_must_be_non_resident",
                    RecordEdit::type_index_allocation, resident_header_bytes,
                    0),
      RootAttribute("index_root_must_be_resident", RecordEdit::type_index_root,
                    non_resident_header_bytes, 1),
      RootAttribute("standard_information_must_be_resident",
                    type_standard_information, non_resident_header_bytes, 1),
      VolumeAttribute("volume_information_must_be_resident",
                      RecordEdit::type_volume_information),
      VolumeAttribute("volume_name_must_be_resident", type_volume_name),
      {.name = "full_cache_index_block_crosses_64kib_block",
       .base = Base,
       .damage =
           [](Image& image) {
             BpbEdit::Apply(image, [](Data::NtfsBpb& bpb) {
               bpb.clusters_per_index_block = shift_17_index_block;
             });
           },
       .probe = Probe::FullCache},
  };
  return inputs;
}

}  // namespace

void LegacyResidencyScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::RegisterDamaged(registry, Inputs());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
