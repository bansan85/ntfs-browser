#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-entry-flag.h"
#include "data/index-root.h"
#include "fake-ntfs-image-internal.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// LCN where BuildFakeNtfsImageWithGapCollationSubNode() writes its own
// index block, past every record's cluster range.
constexpr DWORD gap_collation_index_block_lcn = 20;

}  // namespace

// Writes a resident $INDEX_ROOT at offset holding one FILE_NAME entry named
// name, filed under parent_ref, then the terminating entry. The entry's MFT
// record is mft_index. Returns the attribute's total_size.
DWORD WriteFileNameIndexRootAttr(FakeRecord& record, DWORD offset,
                                 std::wstring_view name, ULONGLONG mft_index,
                                 ULONGLONG parent_ref) {
  auto& attr = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
      &record.at(offset));
  attr.header.type = Attr::Type::IndexRoot;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + attr.attr_offset);
  auto& root =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const std::span<BYTE> entries =
      body.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot));
  // Entry 1: the single real FILE_NAME entry this variant declares.
  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          entries.data());
  first_entry.mft_index = mft_index;
  first_entry.mft_sn = 1;

  auto& filename = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
      &first_entry.stream);
  filename.parent_ref = parent_ref;
  filename.flags = NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::None;
  filename.name_length = gsl::narrow<BYTE>(name.size());
  filename.name_space = NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < filename.name_length; i++) {
    // name_length is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size = gsl::narrow<WORD>(
      reinterpret_cast<BYTE*>(&filename.name[filename.name_length]) -
      reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          entries.subspan(first_entry.size).data());
  second_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size =
      static_cast<DWORD>(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)) +
      first_entry.size + second_entry.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
  return attr.header.total_size;
}

namespace {

// Builds a directory record whose resident $INDEX_ROOT holds one real
// FILE_NAME entry (name, mftIndex, parentRef) plus the terminating entry.
FakeRecord MakeIndexRootDirRecord(std::wstring_view name, ULONGLONG mft_index,
                                  ULONGLONG parent_ref) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);
  const DWORD root_total = WriteFileNameIndexRootAttr(
      record, attr_offset_value, name, mft_index, parent_ref);
  WriteEndOfAttributesMarker(record, attr_offset_value + root_total);
  return record;
}

// Writes the non-terminal "A_" entry of the gap-collation root: a directory
// that is also a sub-node pointer to VCN 0. Returns its size.
WORD WriteGapNonTerminalEntry(std::span<BYTE> entry) {
  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          entry.data());
  first_entry.mft_index = gap_collation_non_terminal_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
      &first_entry.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(Mft::Idx::Root);
  fn1.flags = NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::Directory;
  constexpr std::wstring_view non_terminal_name = L"A_";
  fn1.name_length = 2;
  fn1.name_space = NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < fn1.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(non_terminal_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;
  return first_entry.size;
}

// Writes the terminating entry of the gap-collation root: no name, no
// sub-node. Returns its size.
WORD WriteGapTerminatingEntry(std::span<BYTE> entry) {
  auto& second_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          entry.data());
  second_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));
  return second_entry.size;
}

// Writes the resident $INDEX_ROOT of the gap-collation root at offset: the
// "A_" entry, then the terminating entry. Returns the attribute's total_size.
DWORD WriteGapCollationIndexRootAttr(FakeRecord& record, DWORD offset) {
  auto& root_attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
          &record.at(offset));
  root_attr.header.type = Attr::Type::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Non-terminal, so it carries both a name and a sub-node VCN right after it.
  const std::span<BYTE> entries =
      body.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot));
  const WORD first_size = WriteGapNonTerminalEntry(entries);
  const WORD second_size =
      WriteGapTerminatingEntry(entries.subspan(first_size));

  root.total_entry_size = static_cast<DWORD>(first_size) + second_size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)) +
      first_size + second_size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);
  return root_attr.header.total_size;
}

// Builds a root-directory replacement whose own $INDEX_ROOT holds one
// real, non-terminal FILE_NAME entry that is also a sub-node pointer into
// a real $INDEX_ALLOCATION index block.
FakeRecord MakeRootRecordWithGapCollationSubNode() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);

  DWORD offset = attr_offset_value;
  offset += WriteGapCollationIndexRootAttr(record, offset);
  // $INDEX_ALLOCATION: one cluster, which is VCN 0 only.
  offset += WriteSingleRunIndexAllocAttr(
      record, offset,
      {.start_vcn = 0, .clusters = 1, .lcn = gap_collation_index_block_lcn},
      cluster_size);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Directory record (root, #5): a resident $INDEX_ROOT holding "names" as leaf
// entries, then the terminating entry.
FakeRecord
    MakeIndexRootDirRecordWithNames(std::span<const FakeIndexName> names) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);

  auto& attr = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::IndexRoot;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);
  auto& root =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const std::span<BYTE> entries =
      body.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot));
  DWORD leaf_bytes = 0;
  for (const FakeIndexName& name : names) {
    leaf_bytes += WriteFilenameEntry(&gsl::at(entries, leaf_bytes), name);
  }

  auto& last = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
      &gsl::at(entries, leaf_bytes));
  last.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  last.stream_size = 0;
  last.size = gsl::narrow<WORD>(
      AlignAttrSize(&last.stream - reinterpret_cast<BYTE*>(&last)));

  root.total_entry_size = leaf_bytes + last.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size =
      static_cast<DWORD>(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)) +
      leaf_bytes + last.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithIndexRootVariants() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(index_root_variant_a_dir_idx,
             MakeIndexRootDirRecord(index_root_variant_a_name,
                                    index_root_variant_a_mft_ref,
                                    index_root_variant_a_dir_idx));
  put_record(index_root_variant_b_dir_idx,
             MakeIndexRootDirRecord(index_root_variant_b_name,
                                    index_root_variant_b_mft_ref,
                                    index_root_variant_b_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithRootIndexRootEntry() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeIndexRootExtensionRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithGapCollationSubNode() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeRootRecordWithGapCollationSubNode();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  // The sub-node itself: one real index block holding
  // gap_collation_search_name as its only leaf entry.
  const size_t block_offset =
      static_cast<size_t>(gap_collation_index_block_lcn) * cluster_size;
  if (image.size() < block_offset + cluster_size) {
    image.resize(block_offset + cluster_size, 0);
  }

  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
  auto& block = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexBlock*>(
      block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds
  // trivially without a real fixup array.
  block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = 0;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start,
                sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock));

  // Entry 1: the real leaf entry, a plain leaf with no sub-node.
  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(body.data());
  first_entry.mft_index = gap_collation_leaf_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
      &first_entry.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(Mft::Idx::Root);
  fn1.flags = NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::None;
  fn1.name_length = gap_collation_search_name_length;
  fn1.name_space = NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < fn1.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(gap_collation_search_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          &gsl::at(body, first_entry.size));
  second_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  block.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  block.alloc_entry_size = block.total_entry_size;

  return image;
}

namespace {

// LCN of the $INDEX_ALLOCATION block the non-ASCII fixture files its names in.
constexpr DWORD non_ascii_index_block_lcn = 20;

// LCN of $UpCase's data in the non-ASCII fixture, past every record above.
constexpr DWORD non_ascii_up_case_lcn = 64;

// The 128 KiB $UpCase image the non-ASCII fixture stores: the identity map,
// but for a-z, the Latin-1 letters and y-diaeresis. The dotless i stays
// unmapped, as it does in a Windows table. Independent of the library.
std::vector<BYTE> MakeNonAsciiUpCaseBytes() {
  constexpr WORD latin1_lower_first = 0x00E0;
  constexpr WORD latin1_lower_last = 0x00FE;
  constexpr WORD division_sign = 0x00F7;
  constexpr WORD y_diaeresis = 0x00FF;
  constexpr WORD capital_y_diaeresis = 0x0178;
  constexpr WORD case_distance = 0x20;

  std::vector<BYTE> bytes(NTFS_BROWSER_NAMESPACE::UpCase::Table::unit_count *
                          sizeof(WORD));
  for (size_t unit = 0;
       unit < NTFS_BROWSER_NAMESPACE::UpCase::Table::unit_count; unit++) {
    auto upper = gsl::narrow<WORD>(unit);
    if ((unit >= L'a' && unit <= L'z') ||
        (unit >= latin1_lower_first && unit <= latin1_lower_last &&
         unit != division_sign)) {
      upper = gsl::narrow<WORD>(unit - case_distance);
    } else if (unit == y_diaeresis) {
      upper = capital_y_diaeresis;
    }
    bytes.at(unit * 2) = static_cast<BYTE>(upper & byte_mask);
    bytes.at(unit * 2 + 1) = static_cast<BYTE>(upper >> bits_per_byte);
  }
  return bytes;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithNonAsciiNames(NonAsciiNameLayout layout,
                                                      bool with_up_case) {
  // Sorted by uppercase: E-acute (C9), O-diaeresis (D6), dotless i (131).
  const std::array<FakeIndexName, 3> names{
      {{non_ascii_acute_name, non_ascii_acute_mft_ref, false},
       {non_ascii_diaeresis_name, non_ascii_diaeresis_mft_ref, false},
       {non_ascii_dotless_name, non_ascii_dotless_mft_ref, false}}};

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](Mft::Idx idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * static_cast<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  if (layout == NonAsciiNameLayout::IndexRoot) {
    put_record(Mft::Idx::Root, MakeIndexRootDirRecordWithNames(names));
  } else {
    const std::vector<FakeDataRun> runs{{non_ascii_index_block_lcn, 1}};
    put_record(Mft::Idx::Root,
               MakeIndexAllocationDirRecord(
                   NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive,
                   {.comp_unit_size = 0,
                    .real_size = fake_file_record_size,
                    .runs = runs}));
    LayRunBytes(image, runs, MakeIndexBlockContent(names));
  }

  if (with_up_case) {
    // $MFT now maps up to record 10, so $UpCase can be read through it.
    put_record(Mft::Idx::Mft, MakeMftRecordWithRealDataRun(
                                  static_cast<DWORD>(mft_lcn),
                                  static_cast<DWORD>(Mft::Idx::UpCase) + 1));

    const std::vector<BYTE> table = MakeNonAsciiUpCaseBytes();
    const std::vector<FakeDataRun> runs{
        {non_ascii_up_case_lcn,
         gsl::narrow<DWORD>(table.size() / fake_cluster_size)}};

    FakeRecord upcase = MakeRecordHeader(
        attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
    const DWORD attr_size = WriteNonResidentAttr(
        upcase, attr_offset_value, Attr::Type::Data,
        {.comp_unit_size = 0, .real_size = table.size(), .runs = runs});
    WriteEndOfAttributesMarker(upcase, attr_offset_value + attr_size);
    put_record(Mft::Idx::UpCase, upcase);

    LayRunBytes(image, runs, table);
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSurrogatePairNames() {
  const std::array<FakeIndexName, 2> root_names{
      {{surrogate_names.at(0), surrogate_name_mft_refs.at(0),
        surrogate_name_is_directory.at(0)},
       {surrogate_names.at(1), surrogate_name_mft_refs.at(1),
        surrogate_name_is_directory.at(1)}}};
  const std::array<FakeIndexName, 2> block_names{
      {{surrogate_names.at(2), surrogate_name_mft_refs.at(2),
        surrogate_name_is_directory.at(2)},
       {surrogate_names.at(3), surrogate_name_mft_refs.at(3),
        surrogate_name_is_directory.at(3)}}};

  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs},
      root_names);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeIndexBlockContent(block_names)));
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
