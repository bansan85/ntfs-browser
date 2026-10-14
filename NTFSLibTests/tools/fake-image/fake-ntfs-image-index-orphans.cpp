#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/mft/idx.h>

#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-entry-flag.h"
#include "data/index-root.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"
#include "file-record-header-edit.h"

namespace NtfsBrowserTests {

namespace {

// The index block size field's encoding of a size below one cluster:
// 0xF7 is -9, so 2^9 bytes.
constexpr BYTE sub_cluster_index_block_encoding = 0xF7;

// First file record of the index leaf blocks the sub-cluster fixture and the
// split-extent fixture write.
constexpr DWORD sub_cluster_block_record_base = 110;

constexpr DWORD split_block_record_base = 120;

// LCN where BuildFakeNtfsImageWithOrphanedIndexBlocks() writes its three
// index blocks, kept clear of every other fixture's placement in this file.
constexpr DWORD orphaned_blocks_lcn = 200;

// Number of index blocks (VCN 0-2) the fixture's $INDEX_ALLOCATION covers.
constexpr DWORD orphaned_blocks_count = 3;

static_assert(FakeIndexAllocShape{}.mapped_clusters == orphaned_blocks_count);

static_assert(FakeIndexAllocShape{}.last_vcn == orphaned_blocks_count - 1);

static_assert(FakeIndexAllocShape{}.declared_block_count ==
              orphaned_blocks_count);

// Writes the $INDEX_ALLOCATION a shaped root carries at offset. Its VCN range
// and declared size come from shape. Its single data run maps
// shape.mapped_clusters clusters at orphaned_blocks_lcn, with a three-byte
// length field when that count does not fit in one byte. Returns the
// attribute's total_size.
DWORD WriteShapedIndexAllocAttr(FakeRecord& record, DWORD offset,
                                const FakeIndexAllocShape& shape) {
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = Attr::Type::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = shape.start_vcn;
  alloc_attr.last_vcn = shape.last_vcn;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = shape.declared_block_count * cluster_size;
  alloc_attr.alloc_size = alloc_attr.real_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size. A
  // one-byte length caps a run at 255 clusters, so a longer run takes three.
  const BYTE length_bytes = shape.mapped_clusters <= 0xFF ? 1 : 3;
  gsl::at(data_run, run_len++) =
      static_cast<BYTE>(run_header4_lcn_bytes | length_bytes);
  for (BYTE i = 0; i < length_bytes; i++) {
    gsl::at(data_run, run_len++) =
        static_cast<BYTE>(shape.mapped_clusters >> (8U * i));
  }
  {
    const DWORD lcn = orphaned_blocks_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);
  return alloc_attr.header.total_size;
}

// Builds a root-directory replacement whose $INDEX_ROOT points only at
// VCN 0, while its $INDEX_ALLOCATION stream is shaped by shape. The default
// shape maps orphaned_blocks_count blocks, so VCN 1 and 2 exist on "disk" but
// no pointer in the tree reaches them. A declared count different from the
// mapped one forges the attribute's own real_size (hence GetIndexBlockCount())
// without changing the data run.
FakeRecord MakeOrphanedIndexBlocksRootRecord(const FakeIndexAllocShape& shape) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);
  // The sequence its entries' parent references carry.
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.seq_no = root_sequence_number;
  });

  DWORD offset = attr_offset_value;
  // Sole entry: nameless, terminal, pointing at VCN 0 - the only block a
  // normal B+ tree walk reaches in this fixture.
  offset += WriteSubNodeIndexRootAttr(record, offset, cluster_size, 1);
  offset += WriteShapedIndexAllocAttr(record, offset, shape);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes a single-cluster index block at "vcn" (relative to
// orphaned_blocks_lcn) holding one real leaf entry, into "image".
void WriteOrphanedIndexLeafBlock(std::vector<BYTE>& image, DWORD vcn,
                                 ULONGLONG mft_ref, ULONGLONG parent_ref,
                                 std::wstring_view name) {
  const auto name_length = gsl::narrow<BYTE>(name.size());
  const size_t blocks_offset =
      static_cast<size_t>(orphaned_blocks_lcn) * cluster_size;
  const std::span<BYTE> block_start = std::span<BYTE>(image).subspan(
      blocks_offset + static_cast<size_t>(vcn) * cluster_size);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds trivially
  // without a real fixup array.
  block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&first_entry.stream);
  fn1.parent_ref = parent_ref;
  fn1.flags = NtfsBrowser::Data::FilenameFlag::None;
  fn1.name_length = name_length;
  fn1.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_length; i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// LCN where BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock() writes
// its index blocks, kept clear of every other fixture's placement in this
// file.
constexpr DWORD multi_cluster_orphan_lcn = 210;

// Index block size in bytes for that fixture: more than one cluster, so the
// blockIndex-to-VCN scaling under test actually multiplies.
constexpr DWORD multi_cluster_orphan_index_block_size =
    static_cast<DWORD>(multi_cluster_orphan_clusters_per_block) * cluster_size;

// Two real index blocks: block 0 (VCN 0, reachable) and block 1 (VCN
// multi_cluster_orphan_clusters_per_block, orphaned).
constexpr DWORD multi_cluster_orphan_block_count = 2;

// Builds a root-directory replacement whose $INDEX_ROOT points only at
// block 0, while $INDEX_ALLOCATION covers multi_cluster_orphan_block_count
// blocks of multi_cluster_orphan_clusters_per_block clusters each.
FakeRecord MakeMultiClusterOrphanedIndexBlocksRootRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);
  // The sequence its entries' parent references carry.
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.seq_no = root_sequence_number;
  });

  DWORD offset = attr_offset_value;
  // $INDEX_ROOT: a single sub-node pointer at VCN 0 (block 0).
  offset += WriteSubNodeIndexRootAttr(record, offset,
                                      multi_cluster_orphan_index_block_size,
                                      multi_cluster_orphan_clusters_per_block);

  // $INDEX_ALLOCATION: multi_cluster_orphan_block_count contiguous blocks at
  // multi_cluster_orphan_lcn, only block 0 ever pointed at from $INDEX_ROOT.
  const DWORD total_clusters =
      static_cast<DWORD>(multi_cluster_orphan_block_count) *
      multi_cluster_orphan_clusters_per_block;
  offset += WriteSingleRunIndexAllocAttr(
      record, offset,
      {.start_vcn = 0,
       .clusters = total_clusters,
       .lcn = multi_cluster_orphan_lcn},
      static_cast<ULONGLONG>(multi_cluster_orphan_block_count) *
          multi_cluster_orphan_index_block_size);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes a real index block at block index "blockIndex" (VCN blockIndex *
// multi_cluster_orphan_clusters_per_block), holding one real leaf entry.
void WriteMultiClusterIndexLeafBlock(std::vector<BYTE>& image,
                                     DWORD block_index, ULONGLONG mft_ref,
                                     ULONGLONG parent_ref,
                                     std::wstring_view name) {
  const auto name_length = gsl::narrow<BYTE>(name.size());
  const ULONGLONG vcn = static_cast<ULONGLONG>(block_index) *
                        multi_cluster_orphan_clusters_per_block;
  const size_t blocks_offset =
      static_cast<size_t>(multi_cluster_orphan_lcn) * cluster_size;
  const std::span<BYTE> block_start = std::span<BYTE>(image).subspan(
      blocks_offset + gsl::narrow<size_t>(vcn) * cluster_size);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points the USA at the block's own last (1 + sectors) words, so every
  // PatchUS() check compares a still-zero byte range to itself and the
  // whole array trivially self-patches, without a real fixup array - the
  // multi-sector generalization of the single-cluster fixtures' same trick.
  const DWORD sectors =
      multi_cluster_orphan_index_block_size / bytes_per_sector;
  block.offset_of_us = gsl::narrow<WORD>(multi_cluster_orphan_index_block_size -
                                         2 * (1 + sectors));
  block.size_of_us = gsl::narrow<WORD>(1 + sectors);
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&first_entry.stream);
  fn1.parent_ref = parent_ref;
  fn1.flags = NtfsBrowser::Data::FilenameFlag::None;
  fn1.name_length = name_length;
  fn1.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_length; i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// Builds a root-directory replacement, sequence root_sequence_number, whose
// $INDEX_ROOT points only at block 0. Its $INDEX_ALLOCATION has one instance
// per extent. The first declares declaredBlockCount blocks of ibSize bytes.
// The others declare no size, as a real continuation does.
FakeRecord MakeDirectoryWithIndexAllocation(
    DWORD ib_size, std::span<const FakeIndexAllocExtent> extents,
    ULONGLONG declared_block_count) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.seq_no = root_sequence_number;
  });

  DWORD offset = attr_offset_value;
  offset += WriteSubNodeIndexRootAttr(
      record, offset, ib_size,
      gsl::narrow<BYTE>(ib_size >= cluster_size ? ib_size / cluster_size : 1));

  bool first = true;
  for (const FakeIndexAllocExtent& extent : extents) {
    offset += WriteSingleRunIndexAllocAttr(
        record, offset, extent, first ? declared_block_count * ib_size : 0);
    first = false;
  }

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Where an index block WriteIndexLeafBlockAt() writes sits, and what it holds.
struct FakeIndexLeafBlock {
  // Byte offset of the block in the image.
  size_t offset;
  // Size of the block in bytes.
  DWORD size;
  // VCN the block header declares.
  ULONGLONG vcn;
  // MFT record the block's single leaf entry points to.
  ULONGLONG mft_ref;
  // Parent directory reference that entry carries.
  ULONGLONG parent_ref;
  // Name of that entry.
  std::wstring_view name;
};

// Writes an index block holding one real leaf entry, as spec describes.
void WriteIndexLeafBlockAt(std::vector<BYTE>& image,
                           const FakeIndexLeafBlock& spec) {
  const size_t block_offset = spec.offset;
  const DWORD block_size = spec.size;
  const ULONGLONG vcn = spec.vcn;
  const ULONGLONG mft_ref = spec.mft_ref;
  const ULONGLONG parent_ref = spec.parent_ref;
  const std::wstring_view name = spec.name;
  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points the USA at the block's own last (1 + sectors) words, so PatchUS()
  // compares a still-zero range to itself, without a real fixup array.
  const DWORD sectors = block_size / bytes_per_sector;
  block.offset_of_us = gsl::narrow<WORD>(block_size - 2 * (1 + sectors));
  block.size_of_us = gsl::narrow<WORD>(1 + sectors);
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&first_entry.stream);
  fn1.parent_ref = parent_ref;
  fn1.flags = NtfsBrowser::Data::FilenameFlag::None;
  fn1.name_length = gsl::narrow<BYTE>(name.size());
  fn1.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  for (size_t i = 0; i < name.size(); i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[name.size()]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// Clusters BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks()'s
// $INDEX_ALLOCATION maps: two 512-byte blocks per 1024-byte cluster, so four
// blocks in all.
constexpr DWORD sub_cluster_alloc_clusters = 2;

// Clusters each of BuildFakeNtfsImageWithSplitIndexAllocation()'s two
// $INDEX_ALLOCATION instances maps: one block per cluster, so four blocks in
// all.
constexpr DWORD split_extent_clusters = 2;

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlocks() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record =
      MakeOrphanedIndexBlocksRootRecord(FakeIndexAllocShape{});
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset =
      static_cast<size_t>(orphaned_blocks_lcn) * cluster_size;
  const size_t blocks_bytes =
      static_cast<size_t>(orphaned_blocks_count) * cluster_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end = blocks_offset + blocks_bytes;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);

  // VCN 0: reachable through $INDEX_ROOT's own sub-node pointer.
  WriteOrphanedIndexLeafBlock(image, 0, orphaned_block_reachable_mft_ref,
                              root_ref, orphaned_block_reachable_name);
  // VCN 1: orphaned, but still filed under this directory.
  WriteOrphanedIndexLeafBlock(image, 1, orphaned_block_orphan_mft_ref, root_ref,
                              orphaned_block_orphan_name);
  // VCN 2: orphaned, and filed under a different parent - a recovery scan
  // must find the block but reject the entry.
  WriteOrphanedIndexLeafBlock(
      image, 2, orphaned_block_stale_mft_ref,
      MakeFileReference(orphaned_block_stale_parent_ref, root_sequence_number),
      orphaned_block_stale_name);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeOrphanScanBlockCount() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Replace the root directory's (#5) $INDEX_ALLOCATION real_size only: the
  // three real blocks (and $INDEX_ROOT's own pointer to VCN 0) stay exactly
  // as BuildFakeNtfsImageWithOrphanedIndexBlocks() wrote them.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record =
      MakeOrphanedIndexBlocksRootRecord(FakeIndexAllocShape{
          .declared_block_count = huge_orphan_scan_declared_block_count});
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithIndexAllocationShape(FakeIndexAllocShape shape) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Only the root's $INDEX_ALLOCATION changes: the three real blocks, and
  // $INDEX_ROOT's pointer to VCN 0, stay as the base fixture wrote them.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeOrphanedIndexBlocksRootRecord(shape);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Give orphaned_block_sequence_mismatch_target_idx a real, in-use record on
  // disk, under orphaned_block_sequence_mismatch_record_seq.
  const FakeRecord record =
      MakeMftTreeRecord(NtfsBrowser::Data::FileRecordFlag::InUse,
                        orphaned_block_sequence_mismatch_record_seq, {});
  PutMftRecord(image, static_cast<DWORD>(mft_lcn) * cluster_size,
               orphaned_block_sequence_mismatch_target_idx, record);

  // Redirect the "Orphan" entry (VCN 1) to name that record instead of
  // orphaned_block_orphan_mft_ref, leaving its mft_sn exactly as
  // WriteOrphanedIndexLeafBlock() wrote it (1) - now a mismatch against the
  // record's own sequence number (orphaned_block_sequence_mismatch_record_seq).
  const size_t vcn1_offset =
      (static_cast<size_t>(orphaned_blocks_lcn) + 1) * cluster_size;
  auto& entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &image.at(vcn1_offset + sizeof(NtfsBrowser::Data::IndexBlock)));
  entry.mft_index = orphaned_block_sequence_mismatch_target_idx;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Only this fixture's directory needs an index block bigger than 1
  // cluster - patch the shared BPB in place, the same trick
  // BuildFakeNtfsImageWithForgedIndexBlock() uses.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block =
      static_cast<DWORD>(multi_cluster_orphan_clusters_per_block);

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeMultiClusterOrphanedIndexBlocksRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset =
      static_cast<size_t>(multi_cluster_orphan_lcn) * cluster_size;
  const size_t blocks_bytes =
      static_cast<size_t>(multi_cluster_orphan_block_count) *
      multi_cluster_orphan_index_block_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end = blocks_offset + blocks_bytes;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);

  WriteMultiClusterIndexLeafBlock(image, 0, multi_cluster_reachable_mft_ref,
                                  root_ref, multi_cluster_reachable_name);
  WriteMultiClusterIndexLeafBlock(image, 1, multi_cluster_orphan_mft_ref,
                                  root_ref, multi_cluster_orphan_name);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB: index blocks of 2^9 bytes, half a cluster.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = sub_cluster_index_block_encoding;

  constexpr DWORD index_block_size = 512;
  constexpr DWORD alloc_lcn = 220;
  const std::array<FakeIndexAllocExtent, 1> extents{
      FakeIndexAllocExtent{.start_vcn = 0,
                           .clusters = sub_cluster_alloc_clusters,
                           .lcn = alloc_lcn}};
  const FakeRecord record = MakeDirectoryWithIndexAllocation(
      index_block_size, extents, sub_cluster_block_names.size());
  const size_t root_offset = static_cast<size_t>(mft_lcn) * cluster_size +
                             static_cast<size_t>(fake_file_record_size) *
                                 static_cast<size_t>(Mft::Idx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset = static_cast<size_t>(alloc_lcn) * cluster_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end =
      blocks_offset + sub_cluster_block_names.size() * index_block_size;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  for (size_t i = 0; i < sub_cluster_block_names.size(); i++) {
    WriteIndexLeafBlockAt(image,
                          {.offset = blocks_offset + i * index_block_size,
                           .size = index_block_size,
                           .vcn = i,
                           .mft_ref = sub_cluster_block_record_base + i,
                           .parent_ref = root_ref,
                           .name = sub_cluster_block_names.at(i)});
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitIndexAllocation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Index blocks are one cluster each: block i is VCN i.
  constexpr DWORD first_lcn = 230;
  constexpr DWORD second_lcn = 240;
  const std::array<FakeIndexAllocExtent, 2> extents{
      FakeIndexAllocExtent{
          .start_vcn = 0, .clusters = split_extent_clusters, .lcn = first_lcn},
      FakeIndexAllocExtent{.start_vcn = split_extent_clusters,
                           .clusters = split_extent_clusters,
                           .lcn = second_lcn}};
  const FakeRecord record = MakeDirectoryWithIndexAllocation(
      cluster_size, extents, split_block_names.size());
  const size_t root_offset = static_cast<size_t>(mft_lcn) * cluster_size +
                             static_cast<size_t>(fake_file_record_size) *
                                 static_cast<size_t>(Mft::Idx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end =
      static_cast<size_t>(second_lcn + split_extent_clusters) * cluster_size;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  for (size_t i = 0; i < split_block_names.size(); i++) {
    const size_t lcn = (i < split_extent_clusters)
                           ? first_lcn + i
                           : second_lcn + (i - split_extent_clusters);
    WriteIndexLeafBlockAt(image, {.offset = lcn * cluster_size,
                                  .size = cluster_size,
                                  .vcn = i,
                                  .mft_ref = split_block_record_base + i,
                                  .parent_ref = root_ref,
                                  .name = split_block_names.at(i)});
  }

  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithOrphanedIndexBlockParentLink(FakeParentLink link) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  const size_t root_offset = static_cast<size_t>(mft_lcn) * cluster_size +
                             static_cast<size_t>(fake_file_record_size) *
                                 static_cast<size_t>(Mft::Idx::Root);
  EditFileRecordHeader(
      std::span(image).subspan(root_offset, fake_file_record_size),
      [&](FileRecordHeader& header) {
        header.seq_no = link.record_sequence;
        header.flags = link.record_in_use
                           ? (NtfsBrowser::Data::FileRecordFlag::InUse |
                              NtfsBrowser::Data::FileRecordFlag::Dir)
                           : NtfsBrowser::Data::FileRecordFlag::Dir;
      });

  // VCN 1's entry names the directory unchecked (sequence 0), so it is
  // reported whatever generation the record is on.
  WriteOrphanedIndexLeafBlock(
      image, 1, orphaned_block_orphan_mft_ref,
      MakeFileReference(static_cast<ULONGLONG>(Mft::Idx::Root), 0),
      orphaned_block_orphan_name);

  // Replace VCN 2's foreign-parent entry: same directory, this generation.
  WriteOrphanedIndexLeafBlock(
      image, 2, orphaned_block_generation_mft_ref,
      MakeFileReference(static_cast<ULONGLONG>(Mft::Idx::Root),
                        link.entry_parent_sequence),
      orphaned_block_generation_name);

  return image;
}

}  // namespace NtfsBrowserTests
