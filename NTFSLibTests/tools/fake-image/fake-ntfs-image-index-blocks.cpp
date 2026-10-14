#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "fake-ntfs-image-internal.h"

namespace NtfsBrowserTests {

namespace {

// A forged $FILE_NAME length that claims more characters than fit.
constexpr BYTE overlong_name_length = 200;

// LCN for the forged index block, placed past every record's cluster range.
constexpr DWORD forged_index_block_lcn = 20;

// Builds a directory record whose $INDEX_ALLOCATION references a single
// forged index block, reached through TraverseSubNode() without real
// B+-tree comparisons.
FakeRecord MakeIndexAllocDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  DWORD offset = attr_offset_value;

  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(&record.at(offset));
  root_attr.header.type = Attr::Type::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = forged_index_block_size;
  root.clusters_per_ib =
      static_cast<BYTE>(forged_index_block_size / cluster_size);
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::SubNode |
                      NtfsBrowser::Data::IndexEntryFlag::Last;
  // Size covers the header up to stream, plus the 8-byte subnode VCN.
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Data::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = Attr::Type::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn =
      static_cast<ULONGLONG>(forged_index_block_size / cluster_size) - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = forged_index_block_size;
  alloc_attr.alloc_size = forged_index_block_size;
  alloc_attr.ini_size = forged_index_block_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = 4-byte LCN offset field; low nibble = 1-byte run length.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) =
      static_cast<BYTE>(forged_index_block_size / cluster_size);
  {
    const DWORD lcn = forged_index_block_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);

  offset += alloc_attr.header.total_size;

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
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  DWORD offset = attr_offset_value;

  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(&record.at(offset));
  root_attr.header.type = Attr::Type::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::SubNode |
                      NtfsBrowser::Data::IndexEntryFlag::Last;
  // Header plus the 8-byte subnode VCN that replaces "stream" when empty.
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Data::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION: one data run, index_block_chain_length clusters starting
  // at index_block_chain_lcn.
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = Attr::Type::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = index_block_chain_length - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = ULONGLONG{index_block_chain_length} * cluster_size;
  alloc_attr.alloc_size = alloc_attr.real_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // Data run header byte: high nibble = LCN offset field size (4 bytes),
  // low nibble = length field size (1 byte) - standard NTFS run encoding
  // (AttrNonResident::PickData, src/attr/non-resident.cpp).
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = static_cast<BYTE>(index_block_chain_length);
  {
    const DWORD lcn = index_block_chain_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);

  offset += alloc_attr.header.total_size;

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
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
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

  auto& block = *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(
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
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = tiny_clusters_per_index_block;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = oversized_clusters_per_index_block;

  return image;
}

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
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the chain's real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t chain_end = chain_offset + chain_bytes;
  const size_t aligned_chain_end =
      ((chain_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_chain_end) {
    image.resize(aligned_chain_end, 0);
  }

  for (DWORD vcn = 0; vcn < index_block_chain_length; vcn++) {
    const std::span<BYTE> block_start = std::span<BYTE>(image).subspan(
        chain_offset + static_cast<size_t>(vcn) * cluster_size);
    auto& block =
        *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
    std::memset(&block, 0, sizeof(block));
    block.magic = index_block_magic;
    // Points offset_of_us at the fixup slot itself, valid for every block.
    block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
    block.size_of_us = 3;
    block.vcn = vcn;
    block.entry_offset = gsl::narrow<DWORD>(
        (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
        reinterpret_cast<BYTE*>(&block.entry_offset));

    const std::span<BYTE> body =
        block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
    auto& first_entry =
        *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());

    const bool is_leaf = (vcn == index_block_chain_length - 1);
    if (!is_leaf) {
      // Intermediate block: a lone, nameless entry pointing at the next VCN.
      block.not_leaf = 1;
      first_entry.mft_index = 0;
      first_entry.mft_sn = 0;
      first_entry.stream_size = 0;
      first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::SubNode |
                          NtfsBrowser::Data::IndexEntryFlag::Last;
      first_entry.size = static_cast<WORD>(AlignAttrSize(
          offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
      auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
      sub_node_vcn = vcn + 1;
    } else {
      // Deepest block: the real, named leaf entry, reached by depth alone.
      block.not_leaf = 0;
      first_entry.mft_index = index_block_chain_leaf_mft_ref;
      first_entry.mft_sn = 1;

      auto& fn1 =
          *reinterpret_cast<NtfsBrowser::Data::Filename*>(&first_entry.stream);
      fn1.parent_ref = static_cast<ULONGLONG>(Mft::Idx::Root);
      fn1.flags = NtfsBrowser::Data::FilenameFlag::None;
      fn1.name_length = index_block_chain_leaf_name_length;
      fn1.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
      for (BYTE i = 0; i < fn1.name_length; i++) {
        // i is below name_length, the length of the name.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        fn1.name[i] = gsl::narrow<WORD>(index_block_chain_leaf_name[i]);
      }

      first_entry.stream_size = gsl::narrow<WORD>(
          reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
          reinterpret_cast<BYTE*>(&fn1));
      first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
      first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
          &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
          first_entry.stream_size));
    }

    block.total_entry_size = first_entry.size;
    block.alloc_entry_size = first_entry.size;
  }

  return image;
}

namespace {

// LCN where BuildFakeNtfsImageWithBadIndexBlockEntry() writes its two real,
// sibling index blocks (VCN 0 and VCN 1), clear of every other fixture's
// placement in this file.
constexpr DWORD bad_index_block_lcn = 220;

// Builds a root-directory replacement whose $INDEX_ROOT holds two real
// subnode-pointer entries (VCN 0, VCN 1): both are true B+ tree children,
// reached by the normal walk with no recovery needed.
FakeRecord MakeBadIndexBlockEntryRootRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  DWORD offset = attr_offset_value;

  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(&record.at(offset));
  root_attr.header.type = Attr::Type::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: nameless, non-terminal subnode pointer to VCN 0 (the damaged
  // block).
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::SubNode;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  SubNodeVcnSlot(first_entry) = 0;

  // Entry 2: nameless, terminal subnode pointer to VCN 1 (the good block).
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Data::IndexRoot) +
                                        first_entry.size)));
  second_entry.mft_index = 0;
  second_entry.mft_sn = 0;
  second_entry.stream_size = 0;
  second_entry.flags = NtfsBrowser::Data::IndexEntryFlag::SubNode |
                       NtfsBrowser::Data::IndexEntryFlag::Last;
  second_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  SubNodeVcnSlot(second_entry) = 1;

  root.total_entry_size = first_entry.size + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Data::IndexRoot)) +
      root.total_entry_size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION: 2 contiguous blocks at bad_index_block_lcn.
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = Attr::Type::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = 2ULL * cluster_size;
  alloc_attr.alloc_size = alloc_attr.real_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = 2;  // 2 clusters
  {
    const DWORD lcn = bad_index_block_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);
  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes one real (named, non-subnode) leaf entry at entryPtr and returns
// its size.
WORD WriteBadIndexBlockLeafEntry(BYTE* entry_ptr, ULONGLONG mft_ref,
                                 ULONGLONG parent_ref, std::wstring_view name,
                                 bool last) {
  const auto name_length = gsl::narrow<BYTE>(name.size());
  auto& index_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(entry_ptr);
  index_entry.mft_index = mft_ref;
  index_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&index_entry.stream);
  filename.parent_ref = parent_ref;
  filename.flags = NtfsBrowser::Data::FilenameFlag::None;
  filename.name_length = name_length;
  filename.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_length; i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name[i]);
  }

  index_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[name_length]) -
                        reinterpret_cast<BYTE*>(&filename));
  index_entry.flags = last ? NtfsBrowser::Data::IndexEntryFlag::Last
                           : NtfsBrowser::Data::IndexEntryFlag{};
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
      block_offset + sizeof(NtfsBrowser::Data::IndexBlock),
      cluster_size - sizeof(NtfsBrowser::Data::IndexBlock));
}

// Writes the block header (magic, fixup, entry_offset) shared by both of
// BuildFakeNtfsImageWithBadIndexBlockEntry()'s blocks, at VCN vcn (relative
// to bad_index_block_lcn).
NtfsBrowser::Data::IndexBlock&
    WriteBadIndexBlockHeader(std::vector<BYTE>& image, DWORD vcn) {
  const size_t block_offset =
      (static_cast<size_t>(bad_index_block_lcn) + vcn) * cluster_size;
  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
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
  return block;
}

// VCN 0: one real entry ("First"), then an entry whose declared size
// overruns the block - AttrIndexAlloc::ParseIndexBlock() must reject the
// whole block when strict, but keep "First" (parsed before the bad entry)
// when recovering.
void WriteBadIndexBlockDamagedBlock(std::vector<BYTE>& image,
                                    ULONGLONG parent_ref) {
  NtfsBrowser::Data::IndexBlock& block = WriteBadIndexBlockHeader(image, 0);
  const std::span<BYTE> body = BadIndexBlockBody(image, 0);

  const WORD size_a = WriteBadIndexBlockLeafEntry(
      body.data(), bad_index_block_first_mft_ref, parent_ref,
      bad_index_block_first_name, /*last=*/false);

  auto& entry_b =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(body, size_a));
  entry_b.mft_index = 0;
  entry_b.mft_sn = 0;
  entry_b.stream_size = 0;
  entry_b.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
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
  NtfsBrowser::Data::IndexBlock& block = WriteBadIndexBlockHeader(image, 1);
  const std::span<BYTE> body = BadIndexBlockBody(image, 1);

  const WORD size_good =
      WriteBadIndexBlockLeafEntry(body.data(), bad_index_block_good_mft_ref,
                                  parent_ref, bad_index_block_good_name,
                                  /*last=*/true);

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
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  DWORD offset = attr_offset_value;

  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(&record.at(offset));
  root_attr.header.type = Attr::Type::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot)));
  first_entry.mft_index = malformed_index_entry_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&first_entry.stream);
  fn1.parent_ref = MakeFileReference(static_cast<ULONGLONG>(Mft::Idx::Root),
                                     root_sequence_number);
  fn1.flags = NtfsBrowser::Data::FilenameFlag::None;
  fn1.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;

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
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Data::IndexRoot)) +
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
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
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

}  // namespace NtfsBrowserTests
