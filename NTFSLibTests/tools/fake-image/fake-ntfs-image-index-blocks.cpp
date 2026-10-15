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
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// A forged $FILE_NAME length that claims more characters than fit.
constexpr BYTE overlong_name_length = 200;

// LCN for the forged index block, placed past every record's cluster range.
constexpr DWORD forged_index_block_lcn = 20;

}  // namespace

// Writes a resident $INDEX_ROOT at offset whose only entry is a sub-node
// pointer, with index blocks of ib_size bytes spanning clusters_per_ib
// clusters each. Returns the attribute's total_size.
DWORD WriteSubNodeIndexRootAttr(FakeRecord& record, DWORD offset, DWORD ib_size,
                                BYTE clusters_per_ib) {
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
  root.ib_size = ib_size;
  root.clusters_per_ib = clusters_per_ib;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          &gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode |
                      NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  // Header plus the 8-byte subnode VCN that replaces "stream" when empty.
  first_entry.size = static_cast<WORD>(
      AlignAttrSize(offsetof(NTFS_BROWSER_NAMESPACE::Data::IndexEntry, stream) +
                    sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);
  return root_attr.header.total_size;
}

// Writes a non-resident $INDEX_ALLOCATION at offset with one data run, as
// extent describes. It holds real_size bytes of index data. Returns the
// attribute's total_size.
DWORD WriteSingleRunIndexAllocAttr(FakeRecord& record, DWORD offset,
                                   const FakeIndexAllocExtent& extent,
                                   ULONGLONG real_size) {
  auto& alloc_attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &record.at(offset));
  alloc_attr.header.type = Attr::Type::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = extent.start_vcn;
  alloc_attr.last_vcn = extent.start_vcn + extent.clusters - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = real_size;
  alloc_attr.alloc_size = real_size;
  alloc_attr.ini_size = real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // Data run header byte: high nibble = LCN offset field size (4 bytes),
  // low nibble = length field size (1 byte) - standard NTFS run encoding
  // (AttrNonResident::PickData, src/attr/non-resident.cpp).
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(extent.clusters);
  std::memcpy(&gsl::at(data_run, run_len), &extent.lcn, sizeof(extent.lcn));
  run_len += sizeof(extent.lcn);
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);
  return alloc_attr.header.total_size;
}

namespace {

// Builds a directory record whose $INDEX_ALLOCATION references a single
// forged index block, reached through TraverseSubNode() without real
// B+-tree comparisons.
FakeRecord MakeIndexAllocDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);

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
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);

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

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t dir_offset =
      mft_addr +
      static_cast<size_t>(fake_file_record_size) * index_alloc_dir_idx;
  const FakeRecord dir_record = MakeIndexAllocDirRecord();
  std::memcpy(&image.at(dir_offset), dir_record.data(), dir_record.size());

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

// Grows image so it extends past end, to the next whole 64KiB block.
// FullCache always reads a whole 64KiB-aligned block, so a short image fails
// its last read outright.
void PadToFullCacheRead(std::vector<BYTE>& image, size_t end) {
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t aligned_end =
      ((end + full_cache_read_block_size - 1) / full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_end) {
    image.resize(aligned_end, 0);
  }
}

// Writes the header of chain block vcn at block_start, and returns it.
NTFS_BROWSER_NAMESPACE::Data::IndexBlock&
    WriteChainBlockHeader(std::span<BYTE> block_start, DWORD vcn) {
  auto& block = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexBlock*>(
      block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points offset_of_us at the fixup slot itself, valid for every block.
  block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start,
                sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  return block;
}

// Writes the lone, nameless entry of an intermediate chain block, pointing at
// next_vcn. Returns its size.
WORD WriteChainSubNodeEntry(std::span<BYTE> body, ULONGLONG next_vcn) {
  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(body.data());
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode |
                      NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  first_entry.size = static_cast<WORD>(
      AlignAttrSize(offsetof(NTFS_BROWSER_NAMESPACE::Data::IndexEntry, stream) +
                    sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = next_vcn;
  return first_entry.size;
}

// Writes the real, named leaf entry of the deepest chain block. Returns its
// size.
WORD WriteChainLeafEntry(std::span<BYTE> body) {
  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(body.data());
  first_entry.mft_index = index_block_chain_leaf_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
      &first_entry.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(Mft::Idx::Root);
  fn1.flags = NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::None;
  fn1.name_length = index_block_chain_leaf_name_length;
  fn1.name_space = NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < fn1.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(index_block_chain_leaf_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));
  return first_entry.size;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithDeepIndexBlockChain() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeIndexBlockChainRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

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
        WriteChainBlockHeader(block_start, vcn);
    const std::span<BYTE> body =
        block_start.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock));

    const bool is_leaf = (vcn == index_block_chain_length - 1);
    const WORD entry_size = is_leaf ? WriteChainLeafEntry(body)
                                    : WriteChainSubNodeEntry(body, vcn + 1);
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

// Writes the two nameless sub-node entries of a root record: a non-terminal
// one pointing at VCN 0, the damaged block, then a terminal one pointing at
// VCN 1, the good block. Returns their combined size.
WORD WriteTwoSubNodeEntries(std::span<BYTE> entries) {
  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          entries.data());
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode;
  first_entry.size = static_cast<WORD>(
      AlignAttrSize(offsetof(NTFS_BROWSER_NAMESPACE::Data::IndexEntry, stream) +
                    sizeof(ULONGLONG)));
  SubNodeVcnSlot(first_entry) = 0;

  auto& second_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          entries.subspan(first_entry.size).data());
  second_entry.mft_index = 0;
  second_entry.mft_sn = 0;
  second_entry.stream_size = 0;
  second_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode |
                       NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  second_entry.size = static_cast<WORD>(
      AlignAttrSize(offsetof(NTFS_BROWSER_NAMESPACE::Data::IndexEntry, stream) +
                    sizeof(ULONGLONG)));
  SubNodeVcnSlot(second_entry) = 1;

  return static_cast<WORD>(first_entry.size + second_entry.size);
}

// Writes a resident $INDEX_ROOT at offset holding the two sub-node entries of
// WriteTwoSubNodeEntries(), with one-cluster index blocks. Returns the
// attribute's total_size.
DWORD WriteTwoSubNodeIndexRootAttr(FakeRecord& record, DWORD offset) {
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
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const WORD entries_size = WriteTwoSubNodeEntries(
      body.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)));
  root.total_entry_size = entries_size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)) +
      root.total_entry_size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);
  return root_attr.header.total_size;
}

// Builds a root-directory replacement whose $INDEX_ROOT holds two real
// subnode-pointer entries (VCN 0, VCN 1): both are true B+ tree children,
// reached by the normal walk with no recovery needed.
FakeRecord MakeBadIndexBlockEntryRootRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);

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

// One real (named, non-subnode) leaf entry, as WriteBadIndexBlockLeafEntry()
// writes it.
struct FakeBadLeafEntry {
  // MFT record the entry points to.
  ULONGLONG mft_ref;
  // Parent directory reference the entry's $FILE_NAME carries.
  ULONGLONG parent_ref;
  std::wstring_view name;
  // Whether the entry carries the Last flag, which ends its block.
  bool last;
};

// Sequence number every leaf entry of this fixture carries in its reference.
// The "Good" record is built under the same number, so the entry finds it live.
constexpr WORD bad_index_block_leaf_sequence = 1;

// Writes one real leaf entry at entry_ptr and returns its size.
WORD WriteBadIndexBlockLeafEntry(BYTE* entry_ptr,
                                 const FakeBadLeafEntry& spec) {
  const auto name_length = gsl::narrow<BYTE>(spec.name.size());
  auto& index_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(entry_ptr);
  index_entry.mft_index = spec.mft_ref;
  index_entry.mft_sn = bad_index_block_leaf_sequence;

  auto& filename = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
      &index_entry.stream);
  filename.parent_ref = spec.parent_ref;
  filename.flags = NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::None;
  filename.name_length = name_length;
  filename.name_space = NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_length; i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(spec.name[i]);
  }

  index_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[name_length]) -
                        reinterpret_cast<BYTE*>(&filename));
  index_entry.flags = spec.last
                          ? NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last
                          : NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag{};
  index_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &index_entry.stream - reinterpret_cast<BYTE*>(&index_entry) +
      index_entry.stream_size));
  return index_entry.size;
}

// Returns the bytes of bad index block "vcn" that follow its header.
std::span<BYTE> BadIndexBlockBody(std::vector<BYTE>& image, DWORD vcn) {
  const size_t block_offset =
      (static_cast<size_t>(bad_index_block_lcn) + vcn) * cluster_size;
  return std::span<BYTE>(image).subspan(
      block_offset + sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock),
      cluster_size - sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock));
}

// Writes the block header (magic, fixup, entry_offset) shared by both of
// BuildFakeNtfsImageWithBadIndexBlockEntry()'s blocks, at VCN vcn (relative
// to bad_index_block_lcn).
NTFS_BROWSER_NAMESPACE::Data::IndexBlock&
    WriteBadIndexBlockHeader(std::vector<BYTE>& image, DWORD vcn) {
  const size_t block_offset =
      (static_cast<size_t>(bad_index_block_lcn) + vcn) * cluster_size;
  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
  auto& block = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexBlock*>(
      block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds trivially
  // without a real fixup array.
  block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start,
                sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;
  return block;
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

  const WORD size_a = WriteBadIndexBlockLeafEntry(
      body.data(), {.mft_ref = bad_index_block_first_mft_ref,
                    .parent_ref = parent_ref,
                    .name = bad_index_block_first_name,
                    .last = false});

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

  const WORD size_good = WriteBadIndexBlockLeafEntry(
      body.data(), {.mft_ref = bad_index_block_good_mft_ref,
                    .parent_ref = parent_ref,
                    .name = bad_index_block_good_name,
                    .last = true});

  block.total_entry_size = size_good;
  block.alloc_entry_size = size_good;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithBadIndexBlockEntry() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeBadIndexBlockEntryRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset =
      static_cast<size_t>(bad_index_block_lcn) * cluster_size;
  const size_t blocks_bytes = 2ULL * cluster_size;
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

  // Only "Good" gets a record: "First" stays unbuilt, so only recovery reports
  // it. A live record keeps "Good" in strict mode too. The base $MFT maps only
  // the first few records, so its run is widened to reach "Good"'s slot.
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithRealDataRun(static_cast<DWORD>(mft_lcn),
                                            bad_index_block_mft_clusters));
  const FakeRecord good_record = MakeMftTreeRecord(
      NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse,
      bad_index_block_leaf_sequence,
      {{.name = bad_index_block_good_name, .parent_ref = root_ref}});
  PutMftRecord(image, mft_addr, bad_index_block_good_mft_ref, good_record);

  WriteBadIndexBlockDamagedBlock(image, root_ref);
  WriteBadIndexBlockGoodBlock(image, root_ref);

  return image;
}

namespace {

// Builds a root-directory replacement whose $INDEX_ROOT holds a single,
// terminal entry: a real 3-character name is written on disk, but
// name_length claims far more than that.
FakeRecord MakeMalformedIndexEntryFilenameRootRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);

  DWORD offset = attr_offset_value;

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
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(
          &gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)));
  first_entry.mft_index = malformed_index_entry_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
      &first_entry.stream);
  fn1.parent_ref = MakeFileReference(static_cast<ULONGLONG>(Mft::Idx::Root),
                                     root_sequence_number);
  fn1.flags = NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::None;
  fn1.name_space = NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace::Win32;

  // The real, on-disk name is short; stream_size (and hence e1.size) is
  // sized to it, not to the forged name_length below.
  constexpr std::wstring_view real_name = L"Bad";
  constexpr BYTE real_name_length = 3;
  for (BYTE i = 0; i < real_name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(real_name[i]);
  }
  // Claims far more characters than the entry has room for.
  fn1.name_length = overlong_name_length;

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[real_name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithMalformedIndexEntryFilename() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeMalformedIndexEntryFilenameRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());
  return image;
}

namespace {

// Builds a root-directory replacement whose sole resident $DATA attribute's
// total_size reaches exactly to the end of the file record: no bytes are
// left for a trailing Attr::Type::All end-of-attributes marker. The trailing
// bytes stay zero (never written), matching offset_of_us's self-consistent
// fixup trick.
FakeRecord MakeNoEndMarkerRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);

  auto& attr = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::Data;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  // Fills the record exactly to its own end, leaving no room for a marker.
  const DWORD total_size = fake_file_record_size - attr_offset_value;
  attr.attr_size = total_size - static_cast<DWORD>(sizeof(attr));
  attr.header.total_size = total_size;

  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithNoEndMarker() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeNoEndMarkerRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
