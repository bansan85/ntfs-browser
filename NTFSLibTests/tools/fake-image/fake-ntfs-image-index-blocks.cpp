#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// A forged $FILE_NAME length that claims more characters than fit.
constexpr BYTE overlong_name_length = 200;

// LCN for the forged index block, placed past every record's cluster range.
constexpr DWORD forged_index_block_lcn = 20;

}  // namespace

// Writes a non-resident $INDEX_ALLOCATION at offset with one data run, as
// extent describes. It holds real_size bytes of index data. Returns the
// attribute's total_size.
DWORD WriteSingleRunIndexAllocAttr(FakeRecord& record, DWORD offset,
                                   const FakeIndexAllocExtent& extent,
                                   ULONGLONG real_size) {
  auto& alloc_attr =
      InitNonResidentAttr(record, offset, Attr::Type::IndexAllocation);
  alloc_attr.start_vcn = extent.start_vcn;
  alloc_attr.last_vcn = extent.start_vcn + extent.clusters - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = real_size;
  alloc_attr.alloc_size = real_size;
  alloc_attr.ini_size = real_size;

  const DWORD run_len = WriteSingleRunList(
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset),
      extent.clusters, extent.lcn);

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);
  return alloc_attr.header.total_size;
}

namespace {

// Builds a directory record whose $INDEX_ALLOCATION references a single
// forged index block, reached through TraverseSubNode() without real
// B+-tree comparisons.
FakeRecord MakeIndexAllocDirRecord() {
  FakeRecord record = MakeInUseDirectoryRecord();

  DWORD offset = attr_offset_value;
  offset += WriteSubNodeIndexRootAttr(
      record, offset, forged_index_block_size,
      static_cast<BYTE>(forged_index_block_size / cluster_size));
  offset += WriteSingleRunIndexAllocAttr(
      record, offset,
      {.start_vcn = 0,
       .clusters = forged_index_block_size / cluster_size,
       .lcn = forged_index_block_lcn},
      forged_index_block_size);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// LCN where BuildFakeNtfsImageWithDeepIndexBlockChain() writes its chained
// index blocks, kept clear of every other fixture's placement in this file.
constexpr DWORD index_block_chain_lcn = 100;

// Builds a root-directory replacement whose $INDEX_ROOT sub-node pointer
// leads into a index_block_chain_length-block chained $INDEX_ALLOCATION.
FakeRecord MakeIndexBlockChainRootRecord() {
  FakeRecord record = MakeInUseDirectoryRecord();

  DWORD offset = attr_offset_value;
  offset += WriteSubNodeIndexRootAttr(record, offset, cluster_size, 1);
  // $INDEX_ALLOCATION: one data run, index_block_chain_length clusters starting
  // at index_block_chain_lcn.
  offset += WriteSingleRunIndexAllocAttr(record, offset,
                                         {.start_vcn = 0,
                                          .clusters = index_block_chain_length,
                                          .lcn = index_block_chain_lcn},
                                         ULONGLONG{index_block_chain_length} *
                                             cluster_size);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithForgedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Only this fixture's directory needs an index block bigger than 1
  // cluster - patch the shared BPB in place (both fields are DWORD but
  // ntfs-volume.cpp::ParseBootSector() only ever consults their low byte,
  // truncated to a signed char) rather than duplicating
  // BuildFakeNtfsImage()'s whole boot sector setup.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block =
      static_cast<DWORD>(forged_index_block_size / cluster_size);

  PutMftRecord(image, index_alloc_dir_idx, MakeIndexAllocDirRecord());

  const size_t block_offset =
      static_cast<size_t>(forged_index_block_lcn) * cluster_size;
  if (image.size() < block_offset + forged_index_block_size) {
    image.resize(block_offset + forged_index_block_size, 0);
  }

  auto& block = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexBlock*>(
      &image.at(block_offset));
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  block.offset_of_us = forged_index_block_offset_of_us;
  // size_of_us is never checked; this value only keeps the fixture plausible.
  block.size_of_us =
      static_cast<WORD>(forged_index_block_size / bytes_per_sector + 1);
  block.vcn = 0;
  block.not_leaf = 0;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTinyIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = tiny_clusters_per_index_block;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = oversized_clusters_per_index_block;

  return image;
}

namespace {

// The name, mft_ref and parent of the lone entry in the deepest chain block.
FakeIndexName ChainLeafName() {
  return {.name = index_block_chain_leaf_name,
          .mft_ref = index_block_chain_leaf_mft_ref,
          .directory = false,
          .last = true};
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithDeepIndexBlockChain() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  PutRootRecord(image, MakeIndexBlockChainRootRecord());

  // The chain itself: index_block_chain_length contiguous blocks starting at
  // index_block_chain_lcn, one cluster each.
  const size_t chain_offset =
      static_cast<size_t>(index_block_chain_lcn) * cluster_size;
  const size_t chain_bytes =
      static_cast<size_t>(index_block_chain_length) * cluster_size;
  PadToFullCacheRead(image, chain_offset + chain_bytes);

  for (DWORD vcn = 0; vcn < index_block_chain_length; vcn++) {
    const std::span<BYTE> block_start = std::span<BYTE>(image).subspan(
        chain_offset + static_cast<size_t>(vcn) * cluster_size);
    NTFS_BROWSER_NAMESPACE::Data::IndexBlock& block =
        WriteIndexBlockHeader(block_start, cluster_size, vcn);
    BYTE* const entry =
        &gsl::at(block_start, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock));

    const bool is_leaf = (vcn == index_block_chain_length - 1);
    const WORD entry_size = is_leaf ? WriteFilenameEntry(entry, ChainLeafName())
                                    : WriteSubNodeEntry(entry, vcn + 1, true);
    block.not_leaf = is_leaf ? 0 : 1;
    block.total_entry_size = entry_size;
    block.alloc_entry_size = entry_size;
  }

  return image;
}

namespace {

// LCN where BuildFakeNtfsImageWithBadIndexBlockEntry() writes its two real,
// sibling index blocks (VCN 0 and VCN 1), clear of every other fixture's
// placement in this file.
constexpr DWORD bad_index_block_lcn = 220;

// $MFT clusters the bad-index-block image maps: one per file record here, so
// the run reaches the "Good" record's slot.
constexpr DWORD bad_index_block_mft_clusters =
    static_cast<DWORD>(bad_index_block_good_mft_ref) + 1;

// Writes the resident $INDEX_ROOT of the bad-index-block root at offset: two
// nameless sub-node entries, a non-terminal one pointing at VCN 0, the damaged
// block, then a terminal one pointing at VCN 1, the good block, with
// one-cluster index blocks. Returns the attribute's total_size.
DWORD WriteTwoSubNodeIndexRootAttr(FakeRecord& record, DWORD offset) {
  return WriteResidentIndexRoot(
      record, offset, {}, [](std::span<BYTE> entries) -> DWORD {
        const WORD first_size = WriteSubNodeEntry(entries.data(), 0, false);
        return first_size +
               WriteSubNodeEntry(&gsl::at(entries, first_size), 1, true);
      });
}

// Builds a root-directory replacement whose $INDEX_ROOT holds two real
// subnode-pointer entries (VCN 0, VCN 1): both are true B+ tree children,
// reached by the normal walk with no recovery needed.
FakeRecord MakeBadIndexBlockEntryRootRecord() {
  FakeRecord record = MakeInUseDirectoryRecord();

  DWORD offset = attr_offset_value;
  offset += WriteTwoSubNodeIndexRootAttr(record, offset);
  // $INDEX_ALLOCATION: 2 contiguous blocks at bad_index_block_lcn.
  offset += WriteSingleRunIndexAllocAttr(
      record, offset,
      {.start_vcn = 0, .clusters = 2, .lcn = bad_index_block_lcn},
      2ULL * cluster_size);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Sequence number every leaf entry of this fixture carries in its reference.
// The "Good" record is built under the same number, so the entry finds it live.
constexpr WORD bad_index_block_leaf_sequence = 1;

// One real (named, non-subnode) leaf entry of this fixture's blocks. last is
// whether it carries the Last flag, which ends its block.
FakeIndexName BadLeafEntry(ULONGLONG mft_ref, ULONGLONG parent_ref,
                           std::wstring_view name, bool last) {
  return {.name = name,
          .mft_ref = mft_ref,
          .directory = false,
          .sequence = bad_index_block_leaf_sequence,
          .parent_ref = parent_ref,
          .last = last};
}

// Returns the bytes of bad index block "vcn" that follow its header.
std::span<BYTE> BadIndexBlockBody(std::vector<BYTE>& image, DWORD vcn) {
  const size_t block_offset =
      (static_cast<size_t>(bad_index_block_lcn) + vcn) * cluster_size;
  return std::span<BYTE>(image).subspan(
      block_offset + sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock),
      cluster_size - sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock));
}

// Writes the header of bad index block "vcn", and returns it.
NTFS_BROWSER_NAMESPACE::Data::IndexBlock&
    WriteBadIndexBlockHeader(std::vector<BYTE>& image, DWORD vcn) {
  const size_t block_offset =
      (static_cast<size_t>(bad_index_block_lcn) + vcn) * cluster_size;
  return WriteIndexBlockHeader(std::span<BYTE>(image).subspan(block_offset),
                               cluster_size, vcn);
}

// VCN 0: one real entry ("First"), then an entry whose declared size
// overruns the block - AttrIndexAlloc::ParseIndexBlock() must reject the
// whole block when strict, but keep "First" (parsed before the bad entry)
// when recovering.
void WriteBadIndexBlockDamagedBlock(std::vector<BYTE>& image,
                                    ULONGLONG parent_ref) {
  NTFS_BROWSER_NAMESPACE::Data::IndexBlock& block =
      WriteBadIndexBlockHeader(image, 0);
  const std::span<BYTE> body = BadIndexBlockBody(image, 0);

  const WORD size_a = WriteFilenameEntry(
      body.data(), BadLeafEntry(bad_index_block_first_mft_ref, parent_ref,
                                bad_index_block_first_name, false));

  auto& entry_b = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
      &gsl::at(body, size_a));
  entry_b.mft_index = 0;
  entry_b.mft_sn = 0;
  entry_b.stream_size = 0;
  entry_b.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  // Declares far more than the block actually has left after entry A:
  // ParseIndexBlock()'s "index entry exceeds block bounds" check.
  entry_b.size = static_cast<WORD>(cluster_size);

  block.total_entry_size = static_cast<DWORD>(size_a) + entry_b.size;
  block.alloc_entry_size = block.total_entry_size;
}

// VCN 1: one well-formed, terminal leaf entry ("Good") - a sibling,
// unaffected by the other block's defect.
void WriteBadIndexBlockGoodBlock(std::vector<BYTE>& image,
                                 ULONGLONG parent_ref) {
  NTFS_BROWSER_NAMESPACE::Data::IndexBlock& block =
      WriteBadIndexBlockHeader(image, 1);
  const std::span<BYTE> body = BadIndexBlockBody(image, 1);

  const WORD size_good = WriteFilenameEntry(
      body.data(), BadLeafEntry(bad_index_block_good_mft_ref, parent_ref,
                                bad_index_block_good_name, true));

  block.total_entry_size = size_good;
  block.alloc_entry_size = size_good;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithBadIndexBlockEntry() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  PutRootRecord(image, MakeBadIndexBlockEntryRootRecord());

  const size_t blocks_offset =
      static_cast<size_t>(bad_index_block_lcn) * cluster_size;
  const size_t blocks_bytes = 2ULL * cluster_size;
  PadToFullCacheRead(image, blocks_offset + blocks_bytes);

  const ULONGLONG root_ref = root_file_reference;

  // Only "Good" gets a record: "First" stays unbuilt, so only recovery reports
  // it. A live record keeps "Good" in strict mode too. The base $MFT maps only
  // the first few records, so its run is widened to reach "Good"'s slot.
  PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithRealDataRun(static_cast<DWORD>(mft_lcn),
                                            bad_index_block_mft_clusters));
  const FakeRecord good_record = MakeMftTreeRecord(
      NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse,
      bad_index_block_leaf_sequence,
      {{.name = bad_index_block_good_name, .parent_ref = root_ref}});
  PutMftRecord(image, bad_index_block_good_mft_ref, good_record);

  WriteBadIndexBlockDamagedBlock(image, root_ref);
  WriteBadIndexBlockGoodBlock(image, root_ref);

  return image;
}

namespace {

// Builds a root-directory replacement whose $INDEX_ROOT holds a single,
// terminal entry: a real 3-character name is written on disk, but
// name_length claims far more than that.
FakeRecord MakeMalformedIndexEntryFilenameRootRecord() {
  FakeRecord record = MakeInUseDirectoryRecord();

  const DWORD root_total = WriteResidentIndexRoot(
      record, attr_offset_value, {}, [](std::span<BYTE> entries) -> DWORD {
        // The real, on-disk name is short: stream_size (and hence the entry
        // size) is sized to it, not to the forged name_length below.
        const WORD entry_size = WriteFilenameEntry(
            entries.data(), {.name = L"Bad",
                             .mft_ref = malformed_index_entry_mft_ref,
                             .directory = false,
                             .parent_ref = root_file_reference,
                             .last = true});
        auto& entry =
            *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
                entries.data());
        auto& filename =
            *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
                &entry.stream);
        // Claims far more characters than the entry has room for.
        filename.name_length = overlong_name_length;
        return entry_size;
      });

  WriteEndOfAttributesMarker(record, attr_offset_value + root_total);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithMalformedIndexEntryFilename() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  PutRootRecord(image, MakeMalformedIndexEntryFilenameRootRecord());
  return image;
}

namespace {

// Builds a root-directory replacement whose sole resident $DATA attribute's
// total_size reaches exactly to the end of the file record: no bytes are
// left for a trailing Attr::Type::All end-of-attributes marker. The trailing
// bytes stay zero (never written), matching offset_of_us's self-consistent
// fixup trick.
FakeRecord MakeNoEndMarkerRecord() {
  FakeRecord record = MakeInUseFileRecord();

  auto& attr = InitResidentAttr(record, attr_offset_value, Attr::Type::Data);

  // Fills the record exactly to its own end, leaving no room for a marker.
  const DWORD total_size = fake_file_record_size - attr_offset_value;
  attr.attr_size = total_size - static_cast<DWORD>(sizeof(attr));
  attr.header.total_size = total_size;

  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithNoEndMarker() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  PutRootRecord(image, MakeNoEndMarkerRecord());

  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
