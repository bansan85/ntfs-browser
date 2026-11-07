#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/file-record-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// Records of the extension image whose first name the inputs damage.
constexpr ULONGLONG short_value_record = 16;
constexpr ULONGLONG long_name_record = 19;

// The value length a $FILE_NAME cannot have: the fixed part alone takes 66
// bytes.
constexpr DWORD short_value_length = 32;

// A name length that runs past the end of its attribute.
constexpr BYTE overlong_name_length = 255;

// Sizes that carry the attribute walk past the end of the 1 KiB record.
constexpr DWORD size_past_record_a = 0x3F2;
constexpr DWORD size_past_record_b = 0x368;

// The type code of an attribute NTFS does not define.
constexpr DWORD unknown_attribute_type = 0xC0;

// Root record of the gap collation image, which holds the index allocation.
constexpr ULONGLONG root_record = 5;

// Offset of the index block the gap collation image keeps at cluster 20, of
// its first entry, and of the fields of that entry: its size, its flags.
constexpr std::size_t block_offset = 20 * FakeImage::fake_cluster_size;
constexpr std::size_t first_entry_offset = 0x28;
constexpr std::size_t entry_size_field = 8;
constexpr std::size_t entry_flags_field = 12;
constexpr std::size_t sub_node_vcn_bytes = 8;

// Entry flags: it has a sub-node, and it is the last entry of its block.
constexpr DWORD flag_sub_node_last = 0x03;

// A file name whose value is shorter than the structure, and one whose name
// runs past its attribute.
std::vector<BYTE> BadLengths() {
  std::vector<BYTE> image =
      FakeImage::BuildFakeNtfsImageWithMftExtensionRecord();
  RecordEdit::SetValueLength(image,
                             RecordEdit::Attribute(image, short_value_record,
                                                   RecordEdit::type_file_name),
                             short_value_length);
  RecordEdit::SetFileNameLength(
      image,
      RecordEdit::Attribute(image, long_name_record,
                            RecordEdit::type_file_name),
      overlong_name_length);
  return image;
}

// Two attributes whose size carries the walk past the end of the record.
std::vector<BYTE> WalkRunsPastRecordEnd() {
  std::vector<BYTE> image =
      FakeImage::BuildFakeNtfsImageWithMftExtensionRecord();
  RecordEdit::SetTotalSize(image,
                           RecordEdit::Attribute(image, short_value_record,
                                                 RecordEdit::type_file_name),
                           size_past_record_a);
  RecordEdit::SetTotalSize(image,
                           RecordEdit::Attribute(image, long_name_record,
                                                 RecordEdit::type_file_name),
                           size_past_record_b);
  return image;
}

// A root whose $INDEX_ALLOCATION has a type that NTFS does not define.
std::vector<BYTE> IndexAllocationMissing() {
  std::vector<BYTE> image =
      FakeImage::BuildFakeNtfsImageWithGapCollationSubNode();
  RecordEdit::SetType(image,
                      RecordEdit::Attribute(image, root_record,
                                            RecordEdit::type_index_allocation),
                      unknown_attribute_type);
  return image;
}

// A block whose first entry is the last one and points at the sub-node VCN 0:
// the block itself.
std::vector<BYTE> SubNodeSelfLoop() {
  std::vector<BYTE> image =
      FakeImage::BuildFakeNtfsImageWithGapCollationSubNode();
  const std::size_t entry = block_offset + first_entry_offset;
  const std::size_t entry_size = image.at(entry + entry_size_field);
  RecordEdit::StoreDword(image, entry + entry_flags_field, flag_sub_node_last);
  RecordEdit::StoreQword(image, entry + entry_size - sub_node_vcn_bytes, 0);
  return image;
}

}  // namespace

void FileRecordScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry,
      {
          {.name = "filerecord_filename_bad_lengths", .make = BadLengths},
          {.name = "filerecord_walk_runs_past_record_end",
           .make = WalkRunsPastRecordEnd},
          {.name = "filerecord_index_alloc_missing",
           .make = IndexAllocationMissing},
          {.name = "filerecord_index_subnode_self_loop",
           .make = SubNodeSelfLoop},
      });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
