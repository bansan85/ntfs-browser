#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/filename.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "gap-collation-probe.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// LCN where BuildFakeNtfsImageWithGapCollationSubNode() writes its own
// index block, past every record's cluster range.
constexpr DWORD gap_collation_index_block_lcn = 20;

}  // namespace

namespace {

// Builds a directory record whose resident $INDEX_ROOT holds one real
// FILE_NAME entry (name, mftIndex, parentRef) plus the terminating entry.
FakeRecord MakeIndexRootDirRecord(std::wstring_view name, ULONGLONG mft_index,
                                  ULONGLONG parent_ref) {
  FakeRecord record = MakeInUseDirectoryRecord();
  const DWORD root_total = WriteFileNameIndexRootAttr(
      record, attr_offset_value, name, mft_index, parent_ref);
  WriteEndOfAttributesMarker(record, attr_offset_value + root_total);
  return record;
}

// Writes the resident $INDEX_ROOT of the gap-collation root at offset: the
// non-terminal "A_" entry, a directory that is also a sub-node pointer to
// VCN 0, then the terminating entry. Returns the attribute's total_size.
DWORD WriteGapCollationIndexRootAttr(FakeRecord& record, DWORD offset) {
  return WriteResidentIndexRoot(
      record, offset, {}, [](std::span<BYTE> entries) -> DWORD {
        const WORD first_size = WriteFilenameEntry(
            entries.data(), {.name = L"A_",
                             .mft_ref = gap_collation_non_terminal_mft_ref,
                             .directory = true,
                             .sub_node_vcn = 0});
        return first_size + WriteTerminalEntry(&gsl::at(entries, first_size));
      });
}

// Builds a root-directory replacement whose own $INDEX_ROOT holds one
// real, non-terminal FILE_NAME entry that is also a sub-node pointer into
// a real $INDEX_ALLOCATION index block.
FakeRecord MakeRootRecordWithGapCollationSubNode() {
  FakeRecord record = MakeInUseDirectoryRecord();

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
  FakeRecord record = MakeInUseDirectoryRecord();

  const DWORD root_total = WriteResidentIndexRoot(
      record, attr_offset_value, {}, [names](std::span<BYTE> entries) -> DWORD {
        DWORD leaf_bytes = 0;
        for (const FakeIndexName& name : names) {
          leaf_bytes += WriteFilenameEntry(&gsl::at(entries, leaf_bytes), name);
        }
        return leaf_bytes + WriteTerminalEntry(&gsl::at(entries, leaf_bytes));
      });

  WriteEndOfAttributesMarker(record, attr_offset_value + root_total);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithIndexRootVariants() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  PutMftRecord(image, index_root_variant_a_dir_idx,
               MakeIndexRootDirRecord(index_root_variant_a_name,
                                      index_root_variant_a_mft_ref,
                                      index_root_variant_a_dir_idx));
  PutMftRecord(image, index_root_variant_b_dir_idx,
               MakeIndexRootDirRecord(index_root_variant_b_name,
                                      index_root_variant_b_mft_ref,
                                      index_root_variant_b_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithRootIndexRootEntry() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  PutRootRecord(image, MakeIndexRootExtensionRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithGapCollationSubNode() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  PutRootRecord(image, MakeRootRecordWithGapCollationSubNode());

  // The sub-node itself: one real index block holding
  // gap_collation_search_name as its only leaf entry.
  const size_t block_offset =
      static_cast<size_t>(gap_collation_index_block_lcn) * cluster_size;
  if (image.size() < block_offset + cluster_size) {
    image.resize(block_offset + cluster_size, 0);
  }

  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
  auto& block = WriteIndexBlockHeader(block_start, cluster_size, 0);

  // Entry 1: the real leaf entry, a plain leaf with no sub-node. Entry 2: the
  // terminating entry.
  const size_t entries_offset =
      sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock);
  const WORD leaf_size =
      WriteFilenameEntry(&gsl::at(block_start, entries_offset),
                         {.name = gap_collation_search_name,
                          .mft_ref = gap_collation_leaf_mft_ref,
                          .directory = false});
  const WORD terminal_size =
      WriteTerminalEntry(&gsl::at(block_start, entries_offset + leaf_size));

  block.total_entry_size = static_cast<DWORD>(leaf_size) + terminal_size;
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

  const auto put_record = [&](Mft::Idx idx, const FakeRecord& record) {
    PutMftRecord(image, static_cast<ULONGLONG>(idx), record);
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

    FakeRecord upcase = MakeInUseFileRecord();
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
