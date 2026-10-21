#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/index-entry-flag.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Where the fixup array of an index block sits, and where its entries start.
constexpr size_t fixup_array_offset = 0x28;
constexpr size_t first_entry_offset = 0x40;

// Where the index header starts in the block header: entry_offset and the
// sizes after it count from here.
constexpr size_t index_header_offset = 0x18;

// The update sequence number every block gets.
constexpr WORD block_usn = 0x0003;

// Size of the index entry header, and so of an end entry.
constexpr size_t entry_header_size = 16;

// LCN where the blocks are written, past every other record of the image.
constexpr DWORD blocks_lcn = 120;

// Writes a T at offset of bytes.
template <typename T>
void Put(std::vector<BYTE>& bytes, size_t offset, T value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

// Replaces the last word of each sector by the update sequence number and
// saves it in the fixup array.
void ApplyFixups(std::vector<BYTE>& block) {
  const size_t sectors = block.size() / bytes_per_sector;
  Put<WORD>(block, fixup_array_offset, block_usn);
  for (size_t i = 0; i < sectors; ++i) {
    const size_t last_word = ((i + 1) * bytes_per_sector) - 2;
    std::memcpy(&block.at(fixup_array_offset + 2 + (i * 2)),
                &block.at(last_word), 2);
    Put<WORD>(block, last_word, block_usn);
  }
}

}  // namespace

std::vector<BYTE> BuildFakeIndexBlock(const FakeIndexBlock& block,
                                      DWORD block_size) {
  std::vector<BYTE> bytes(block_size, 0);
  const size_t sectors = block_size / bytes_per_sector;
  if (fixup_array_offset + (2 * (1 + sectors)) > first_entry_offset) {
    throw std::out_of_range("block too big for the fixup array");
  }
  std::memcpy(bytes.data(), "INDX", 4);
  Put<WORD>(bytes, 4, static_cast<WORD>(fixup_array_offset));
  Put<WORD>(bytes, 6, static_cast<WORD>(1 + sectors));
  Put<ULONGLONG>(bytes, 16, block.vcn);

  size_t at = first_entry_offset;
  const auto append = [&](std::span<const BYTE> data) {
    if (at + data.size() > bytes.size()) {
      throw std::out_of_range("entries do not fit in the block");
    }
    std::ranges::copy(data, bytes.begin() + static_cast<std::ptrdiff_t>(at));
    at += data.size();
  };
  for (FakeSlackName entry : block.live) {
    entry.with_entry_header = true;
    append(EncodeSlackName(entry));
  }
  std::vector<BYTE> end_entry(entry_header_size, 0);
  Put<WORD>(end_entry, 8, static_cast<WORD>(entry_header_size));
  Put<WORD>(
      end_entry, 12,
      static_cast<WORD>(NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last));
  append(end_entry);

  Put<DWORD>(bytes, 0x18,
             static_cast<DWORD>(first_entry_offset - index_header_offset));
  Put<DWORD>(bytes, 0x1C, static_cast<DWORD>(at - index_header_offset));
  Put<DWORD>(bytes, 0x20, static_cast<DWORD>(block_size - index_header_offset));
  bytes.at(0x24) = block.not_leaf;
  append(block.slack);

  ApplyFixups(bytes);
  return bytes;
}

std::vector<BYTE> BuildFakeNtfsImageWithIndexBlocks(
    std::span<const std::vector<BYTE>> blocks) {
  if (blocks.empty()) {
    throw std::invalid_argument("no block");
  }
  const auto block_size = static_cast<DWORD>(blocks.front().size());
  const DWORD clusters_per_block = block_size / cluster_size;
  const DWORD clusters = clusters_per_block * static_cast<DWORD>(blocks.size());

  std::vector<BYTE> image = BuildFakeNtfsImage();
  if (clusters_per_block != 1) {
    auto& bpb =
        *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
    bpb.clusters_per_index_block = static_cast<BYTE>(clusters_per_block);
  }

  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.seq_no = root_sequence_number;
  });
  DWORD offset = attr_offset_value;
  offset += WriteSubNodeIndexRootAttr(record, offset, block_size,
                                      static_cast<BYTE>(clusters_per_block));
  offset += WriteSingleRunIndexAllocAttr(
      record, offset, {.start_vcn = 0, .clusters = clusters, .lcn = blocks_lcn},
      ULONGLONG{block_size} * blocks.size());
  WriteEndOfAttributesMarker(record, offset);
  const size_t root_offset = static_cast<size_t>(mft_lcn) * cluster_size +
                             static_cast<size_t>(fake_file_record_size) *
                                 static_cast<size_t>(Mft::Idx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  // FullCache reads whole 64 KiB blocks, so the image must run past the end
  // of the last index block.
  constexpr size_t read_block_size = size_t{64} * 1024;
  const size_t blocks_offset = static_cast<size_t>(blocks_lcn) * cluster_size;
  const size_t end =
      blocks_offset + (static_cast<size_t>(block_size) * blocks.size());
  image.resize(
      std::max(image.size(), ((end + read_block_size - 1) / read_block_size) *
                                 read_block_size),
      0);
  for (size_t i = 0; i < blocks.size(); ++i) {
    std::ranges::copy(blocks[i],
                      image.begin() + static_cast<std::ptrdiff_t>(
                                          blocks_offset + (i * block_size)));
  }
  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
