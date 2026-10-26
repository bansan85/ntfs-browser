#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/ntfs-bpb.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

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
  auto& alloc_attr =
      InitNonResidentAttr(record, offset, Attr::Type::IndexAllocation);
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
  // The sequence its entries' parent references carry.
  FakeRecord record = MakeInUseDirectoryRecord(root_sequence_number);

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
  WriteIndexLeafBlockAt(
      image, {.offset = (static_cast<size_t>(orphaned_blocks_lcn) + vcn) *
                        cluster_size,
              .size = cluster_size,
              .vcn = vcn,
              .mft_ref = mft_ref,
              .parent_ref = parent_ref,
              .name = name});
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
  // The sequence its entries' parent references carry.
  FakeRecord record = MakeInUseDirectoryRecord(root_sequence_number);

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
  WriteIndexLeafBlockAt(
      image, {.offset = (static_cast<size_t>(multi_cluster_orphan_lcn) +
                         static_cast<size_t>(block_index) *
                             multi_cluster_orphan_clusters_per_block) *
                        cluster_size,
              .size = multi_cluster_orphan_index_block_size,
              .vcn = static_cast<ULONGLONG>(block_index) *
                     multi_cluster_orphan_clusters_per_block,
              .mft_ref = mft_ref,
              .parent_ref = parent_ref,
              .name = name});
}

// Builds a root-directory replacement, sequence root_sequence_number, whose
// $INDEX_ROOT points only at block 0. Its $INDEX_ALLOCATION has one instance
// per extent. The first declares declaredBlockCount blocks of ibSize bytes.
// The others declare no size, as a real continuation does.
FakeRecord MakeDirectoryWithIndexAllocation(
    DWORD ib_size, std::span<const FakeIndexAllocExtent> extents,
    ULONGLONG declared_block_count) {
  FakeRecord record = MakeInUseDirectoryRecord(root_sequence_number);

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

// Points the "Orphan" leaf entry (VCN 1) of an orphaned-blocks image at
// mft_ref.
void RedirectOrphanEntry(std::vector<BYTE>& image, ULONGLONG mft_ref) {
  const size_t vcn1_offset =
      (static_cast<size_t>(orphaned_blocks_lcn) + 1) * cluster_size;
  auto& entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(&image.at(
          vcn1_offset + sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock)));
  entry.mft_index = mft_ref;
}

// Clusters BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks()'s
// $INDEX_ALLOCATION maps: two 512-byte blocks per 1024-byte cluster, so four
// blocks in all.
constexpr DWORD sub_cluster_alloc_clusters = 2;

// Clusters each of BuildFakeNtfsImageWithSplitIndexAllocation()'s two
// $INDEX_ALLOCATION instances maps: one block per cluster, so four blocks in
// all.
constexpr DWORD split_extent_clusters = 2;

// LCN where BuildFakeNtfsImageWithOrphanedDirectoryLoop() writes its index
// blocks, kept clear of every other fixture's placement in this file.
constexpr DWORD orphaned_loop_lcn = 250;

// Index blocks (VCN 0 and 1) that fixture's $INDEX_ALLOCATION maps.
constexpr DWORD orphaned_loop_block_count = 2;

// Sequence number Sub carries, in its header and where an entry names it.
constexpr WORD orphaned_loop_sub_sequence = 7;

// Sequence number Docs carries: the one mft_tree_docs_idx has in the base
// fixture, which the orphaned entry must name to stand as live.
constexpr WORD orphaned_loop_docs_sequence = 1;

// Sub's record: its $INDEX_ROOT points only at VCN 0, and its
// $INDEX_ALLOCATION maps orphaned_loop_block_count blocks.
FakeRecord MakeOrphanedLoopSubRecord(ULONGLONG docs_ref) {
  FakeRecord record = MakeInUseDirectoryRecord(orphaned_loop_sub_sequence);

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(record, offset,
                              {.name = L"Sub", .parent_ref = docs_ref}, true);
  offset += WriteSubNodeIndexRootAttr(record, offset, cluster_size, 1);
  offset += WriteSingleRunIndexAllocAttr(
      record, offset,
      {.start_vcn = 0,
       .clusters = orphaned_loop_block_count,
       .lcn = orphaned_loop_lcn},
      static_cast<ULONGLONG>(orphaned_loop_block_count) * cluster_size);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlocks() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  PutRootRecord(image,
                MakeOrphanedIndexBlocksRootRecord(FakeIndexAllocShape{}));

  const size_t blocks_offset =
      static_cast<size_t>(orphaned_blocks_lcn) * cluster_size;
  const size_t blocks_bytes =
      static_cast<size_t>(orphaned_blocks_count) * cluster_size;
  PadToFullCacheRead(image, blocks_offset + blocks_bytes);

  const ULONGLONG root_ref = root_file_reference;

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
  PutRootRecord(image, MakeOrphanedIndexBlocksRootRecord(FakeIndexAllocShape{
                           .declared_block_count =
                               huge_orphan_scan_declared_block_count}));

  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithIndexAllocationShape(FakeIndexAllocShape shape) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Only the root's $INDEX_ALLOCATION changes: the three real blocks, and
  // $INDEX_ROOT's pointer to VCN 0, stay as the base fixture wrote them.
  PutRootRecord(image, MakeOrphanedIndexBlocksRootRecord(shape));

  return image;
}

std::wstring ManyIndexBlocksName(ULONGLONG vcn) {
  return L"Block" + std::to_wstring(vcn);
}

std::vector<BYTE> BuildFakeNtfsImageWithManyIndexBlocks(DWORD block_count) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithIndexAllocationShape(
      FakeIndexAllocShape{.last_vcn = block_count - 1ULL,
                          .mapped_clusters = block_count,
                          .declared_block_count = block_count});

  const size_t blocks_offset =
      static_cast<size_t>(orphaned_blocks_lcn) * cluster_size;
  PadToFullCacheRead(image, blocks_offset + static_cast<size_t>(block_count) *
                                                cluster_size);

  const ULONGLONG root_ref = root_file_reference;
  for (DWORD vcn = 0; vcn < block_count; vcn++) {
    const std::wstring name = ManyIndexBlocksName(vcn);
    WriteIndexLeafBlockAt(
        image,
        {.offset = blocks_offset + static_cast<size_t>(vcn) * cluster_size,
         .size = cluster_size,
         .vcn = vcn,
         .mft_ref = many_index_blocks_mft_ref_base + vcn,
         .parent_ref = root_ref,
         .name = name});
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Give orphaned_block_sequence_mismatch_target_idx a real, in-use record on
  // disk, under orphaned_block_sequence_mismatch_record_seq.
  const FakeRecord record =
      MakeMftTreeRecord(NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse,
                        orphaned_block_sequence_mismatch_record_seq, {});
  PutMftRecord(image, orphaned_block_sequence_mismatch_target_idx, record);

  // Redirect the "Orphan" entry (VCN 1) to name that record instead of
  // orphaned_block_orphan_mft_ref, leaving its mft_sn exactly as
  // WriteOrphanedIndexLeafBlock() wrote it (1) - now a mismatch against the
  // record's own sequence number (orphaned_block_sequence_mismatch_record_seq).
  RedirectOrphanEntry(image, orphaned_block_sequence_mismatch_target_idx);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedEntryZeroRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Record 2 is zero-filled in BuildFakeNtfsImage(), so the redirected entry
  // names a record that never parses.
  RedirectOrphanEntry(image, orphaned_block_sequence_mismatch_target_idx);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedDirectoryLoop() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const ULONGLONG docs_ref =
      MakeFileReference(mft_tree_docs_idx, orphaned_loop_docs_sequence);
  const ULONGLONG sub_ref =
      MakeFileReference(mft_tree_reused_dir_idx, orphaned_loop_sub_sequence);

  // Docs indexes Sub alone, as it does after Sub was moved into it.
  const std::array<FakeIndexName, 1> docs_names{
      {{.name = L"Sub",
        .mft_ref = mft_tree_reused_dir_idx,
        .directory = true,
        .sequence = orphaned_loop_sub_sequence,
        .parent_ref = docs_ref}}};
  PutMftRecord(
      image, mft_tree_docs_idx,
      MakeIndexedDirRecord(
          orphaned_loop_docs_sequence,
          FakeFileName{.name = L"Docs", .parent_ref = root_file_reference},
          docs_names));
  PutMftRecord(image, mft_tree_reused_dir_idx,
               MakeOrphanedLoopSubRecord(docs_ref));

  const size_t blocks_offset =
      static_cast<size_t>(orphaned_loop_lcn) * cluster_size;
  PadToFullCacheRead(image, blocks_offset +
                                static_cast<size_t>(orphaned_loop_block_count) *
                                    cluster_size);

  // VCN 0: reachable, and empty but for its terminating entry.
  auto& empty_block = WriteIndexBlockHeader(
      std::span<BYTE>(image).subspan(blocks_offset), cluster_size, 0);
  const WORD terminal_size = WriteTerminalEntry(&image.at(
      blocks_offset + sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock)));
  empty_block.total_entry_size = terminal_size;
  empty_block.alloc_entry_size = terminal_size;

  // VCN 1: orphaned, and still holds the entry of Docs filed under Sub.
  WriteIndexLeafBlockAt(image, {.offset = blocks_offset + cluster_size,
                                .size = cluster_size,
                                .vcn = 1,
                                .mft_ref = mft_tree_docs_idx,
                                .parent_ref = sub_ref,
                                .name = L"Docs",
                                .directory = true});

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Only this fixture's directory needs an index block bigger than 1
  // cluster - patch the shared BPB in place, the same trick
  // BuildFakeNtfsImageWithForgedIndexBlock() uses.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block =
      static_cast<DWORD>(multi_cluster_orphan_clusters_per_block);

  PutRootRecord(image, MakeMultiClusterOrphanedIndexBlocksRootRecord());

  const size_t blocks_offset =
      static_cast<size_t>(multi_cluster_orphan_lcn) * cluster_size;
  const size_t blocks_bytes =
      static_cast<size_t>(multi_cluster_orphan_block_count) *
      multi_cluster_orphan_index_block_size;
  PadToFullCacheRead(image, blocks_offset + blocks_bytes);

  const ULONGLONG root_ref = root_file_reference;

  WriteMultiClusterIndexLeafBlock(image, 0, multi_cluster_reachable_mft_ref,
                                  root_ref, multi_cluster_reachable_name);
  WriteMultiClusterIndexLeafBlock(image, 1, multi_cluster_orphan_mft_ref,
                                  root_ref, multi_cluster_orphan_name);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB: index blocks of 2^9 bytes, half a cluster.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = sub_cluster_index_block_encoding;

  constexpr DWORD index_block_size = 512;
  constexpr DWORD alloc_lcn = 220;
  const std::array<FakeIndexAllocExtent, 1> extents{
      FakeIndexAllocExtent{.start_vcn = 0,
                           .clusters = sub_cluster_alloc_clusters,
                           .lcn = alloc_lcn}};
  const FakeRecord record = MakeDirectoryWithIndexAllocation(
      index_block_size, extents, sub_cluster_block_names.size());
  PutRootRecord(image, record);

  const size_t blocks_offset = static_cast<size_t>(alloc_lcn) * cluster_size;
  PadToFullCacheRead(image, blocks_offset + sub_cluster_block_names.size() *
                                                index_block_size);

  const ULONGLONG root_ref = root_file_reference;
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
  PutRootRecord(image, record);

  PadToFullCacheRead(image,
                     static_cast<size_t>(second_lcn + split_extent_clusters) *
                         cluster_size);

  const ULONGLONG root_ref = root_file_reference;
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

  EditFileRecordHeader(
      MftRecordBytes(image, static_cast<ULONGLONG>(Mft::Idx::Root)),
      [&](FileRecordHeader& header) {
        header.seq_no = link.record_sequence;
        header.flags =
            link.record_in_use
                ? (NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                   NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir)
                : NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir;
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

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
