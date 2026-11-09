#include "file-record-scenarios.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/header-non-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "legacy-fixtures.h"
#include "record-edit.h"
#include "win-types.h"

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

// The index block that the nested inputs add, one cluster after the first, and
// the entry offset, size and allocation that its header declares.
constexpr std::size_t second_block_offset = 21 * FakeImage::fake_cluster_size;
constexpr DWORD second_block_entries_size = 0x78;
constexpr DWORD second_block_allocation = 0x3E8;

// The array of that block starts where its first entry does, so that its
// update sequence number is the low word of the record number of the entry.
// Each sector ends with that number.
constexpr WORD second_block_array_offset = 0x28;
constexpr WORD second_block_array_words = 3;
constexpr WORD second_block_sequence_number = 0x1E;
constexpr std::size_t first_sector_end = 0x1FE;
constexpr std::size_t second_sector_end = 0x3FE;

// The leaf entry grows by the 8 bytes of a sub-node VCN, and the end entry
// that follows it starts there.
constexpr WORD leaf_with_vcn_bytes = 0x60;
constexpr std::size_t end_entry_offset =
    first_entry_offset + leaf_with_vcn_bytes;
constexpr WORD end_entry_bytes = 0x10;
constexpr WORD end_entry_with_vcn_bytes = 0x18;

// The index allocation spans two clusters, the blocks of VCN 0 and 1.
constexpr ULONGLONG two_blocks = 2;
const std::vector<BYTE> two_cluster_run = {0x41, 0x02, 0x14, 0, 0, 0, 0};

// The sub-node a nested entry points to.
constexpr ULONGLONG nested_vcn = 1;

// The block of VCN 0 files "AC" in the input about an entry, and "AA" in the
// input about the descent, where the leaf block files "AB": the offset of the
// second code unit of the name, and the two code units.
constexpr std::size_t file_name_header_bytes = 0x10 + 0x42;
constexpr std::size_t second_name_unit =
    first_entry_offset + file_name_header_bytes + 2;
constexpr WORD unit_a = 0x41;
constexpr WORD unit_c = 0x43;

// The root with an index allocation of two blocks. The block of VCN 1 is a
// copy of the leaf block, with the header of a block written by NTFS: a real
// update sequence array. Which entry of the block of VCN 0 points down to it
// is for sub_node to say.
enum class Descent : std::uint8_t {
  // The leaf entry has the sub-node, and the end entry has none.
  OnLeafEntry,
  // The end entry has the sub-node, and the leaf entry has none.
  OnEndEntry
};

std::vector<BYTE> NestedSubNode(Descent descent) {
  std::vector<BYTE> image =
      FakeImage::BuildFakeNtfsImageWithGapCollationSubNode();
  const std::size_t allocation = RecordEdit::Attribute(
      image, root_record, RecordEdit::type_index_allocation);
  RecordEdit::Edit<Data::HeaderNonResident>(
      image, allocation, [](auto& header) {
        header.last_vcn = two_blocks - 1;
        header.alloc_size = two_blocks * FakeImage::fake_cluster_size;
        header.real_size = two_blocks * FakeImage::fake_cluster_size;
        header.ini_size = two_blocks * FakeImage::fake_cluster_size;
      });
  RecordEdit::SetRunList(image, allocation, two_cluster_run);

  image.resize(std::max(image.size(),
                        second_block_offset + FakeImage::fake_cluster_size));
  std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(block_offset),
              FakeImage::fake_cluster_size,
              image.begin() + static_cast<std::ptrdiff_t>(second_block_offset));
  RecordEdit::Edit<Data::IndexBlock>(
      image, second_block_offset, [](auto& block) {
        block.vcn = nested_vcn;
        block.offset_of_us = second_block_array_offset;
        block.size_of_us = second_block_array_words;
        block.total_entry_size = second_block_entries_size;
        block.alloc_entry_size = second_block_allocation;
      });
  RecordEdit::StoreWord(image, second_block_offset + first_sector_end,
                        second_block_sequence_number);
  RecordEdit::StoreWord(image, second_block_offset + second_sector_end,
                        second_block_sequence_number);

  RecordEdit::StoreWord(image, block_offset + second_name_unit,
                        descent == Descent::OnEndEntry ? unit_a : unit_c);
  const std::size_t leaf = block_offset + first_entry_offset;
  const std::size_t end = block_offset + end_entry_offset;
  const std::size_t old_end = leaf + image.at(leaf + entry_size_field);
  RecordEdit::Edit<Data::IndexEntry>(image, old_end, [](auto& entry) {
    entry.size = 0;
    entry.flags = static_cast<Data::IndexEntryFlag>(0);
  });
  RecordEdit::Edit<Data::IndexEntry>(image, leaf, [&](auto& entry) {
    entry.size = leaf_with_vcn_bytes;
    if (descent == Descent::OnLeafEntry) {
      entry.flags = Data::IndexEntryFlag::SubNode;
    }
  });
  const bool on_leaf = descent == Descent::OnLeafEntry;
  RecordEdit::Edit<Data::IndexEntry>(image, end, [&](auto& entry) {
    entry.size = on_leaf ? end_entry_bytes : end_entry_with_vcn_bytes;
    entry.flags =
        on_leaf ? Data::IndexEntryFlag::Last
                : Data::IndexEntryFlag::Last | Data::IndexEntryFlag::SubNode;
  });
  const std::size_t vcn_at =
      on_leaf ? leaf + leaf_with_vcn_bytes - sub_node_vcn_bytes
              : end + end_entry_bytes;
  RecordEdit::StoreQword(image, vcn_at, nested_vcn);
  RecordEdit::Edit<Data::IndexBlock>(image, block_offset, [&](auto& block) {
    block.total_entry_size = static_cast<DWORD>(
        sizeof(Data::IndexBlock) - offsetof(Data::IndexBlock, entry_offset) +
        leaf_with_vcn_bytes +
        (on_leaf ? end_entry_bytes : end_entry_with_vcn_bytes));
  });
  return image;
}

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
          {.name = "filerecord_index_nested_subnode_entry",
           .make = [] { return NestedSubNode(Descent::OnLeafEntry); }},
          {.name = "filerecord_index_nested_subnode_descent",
           .make = [] { return NestedSubNode(Descent::OnEndEntry); }},
      });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
