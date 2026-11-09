#include "index-layer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "attr/type.h"
#include "attribute-values.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "layer-line.h"
#include "update-sequence.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace Data = NTFS_BROWSER_NAMESPACE::Data;

// Entries start on a multiple of 8 bytes.
constexpr std::size_t entry_alignment = 8;

// The bytes of an entry before its key.
constexpr std::size_t entry_header_bytes = offsetof(Data::IndexEntry, stream);

// Bytes of the VCN that ends an entry with a sub-node.
constexpr std::size_t vcn_bytes = sizeof(ULONGLONG);

// Bits of the file index in a file reference. The sequence number takes the
// 16 bits above.
constexpr unsigned file_index_bits = 48;

// Where the update sequence array of an index block starts: after the block
// header.
constexpr std::size_t block_array_offset = sizeof(Data::IndexBlock);

// The index header starts here in a block. The offsets of the header count
// from it.
constexpr std::size_t block_index_header =
    offsetof(Data::IndexBlock, entry_offset);

// Where the entries start, counted from the index header, in the blocks that
// Windows writes: after the header, the array of a 4 KiB block and padding.
constexpr ULONGLONG default_block_entry_offset = 0x40;

// The index header of an $INDEX_ROOT starts after the 16 bytes of the root
// header. The offsets of the header count from it.
constexpr std::size_t root_index_header =
    offsetof(Data::IndexRoot, entry_offset);

// The collation rule of file names, and the size of the block that 4 KiB
// clusters give.
constexpr ULONGLONG collate_file_name = 1;
constexpr ULONGLONG default_block_bytes = 4096;

// Rounds size up to the alignment of the entries.
std::size_t Align(std::size_t size) {
  return (size + entry_alignment - 1) / entry_alignment * entry_alignment;
}

}  // namespace

std::vector<BYTE> IndexLayer::Entry(const LayerLine& line) {
  const auto flags = static_cast<BYTE>(line.Number("flags", 0));
  const bool last =
      (flags & static_cast<BYTE>(Data::IndexEntryFlag::Last)) != 0;
  const bool sub_node =
      (flags & static_cast<BYTE>(Data::IndexEntryFlag::SubNode)) != 0;
  const std::vector<BYTE> key =
      last ? std::vector<BYTE>{} : AttributeValues::FileName(line);

  std::vector<BYTE> bytes(Align(entry_header_bytes + key.size()) +
                          (sub_node ? vcn_bytes : 0));
  Data::IndexEntry header{};
  const ULONGLONG ref = line.Number("ref", 0);
  header.mft_index = ref & ((ULONGLONG{1} << file_index_bits) - 1);
  header.mft_sn = ref >> file_index_bits;
  header.size = gsl::narrow<WORD>(bytes.size());
  header.stream_size = gsl::narrow<WORD>(key.size());
  header.flags = static_cast<Data::IndexEntryFlag>(flags);
  std::memcpy(bytes.data(), &header, entry_header_bytes);
  std::ranges::copy(key, bytes.begin() + entry_header_bytes);
  if (sub_node) {
    const ULONGLONG vcn = line.Number("vcn");
    std::memcpy(&bytes[bytes.size() - vcn_bytes], &vcn, vcn_bytes);
  }
  return bytes;
}

std::vector<BYTE> IndexLayer::Entries(const std::vector<LayerLine>& lines) {
  std::vector<BYTE> bytes;
  for (const LayerLine& line : lines) {
    const std::vector<BYTE> entry = Entry(line);
    bytes.insert(bytes.end(), entry.begin(), entry.end());
  }
  return bytes;
}

std::vector<BYTE> IndexLayer::Block(const LayerLine& line,
                                    const std::vector<LayerLine>& entries) {
  const auto size = gsl::narrow<std::size_t>(line.Number("size"));
  std::vector<BYTE> bytes(size);
  const std::vector<BYTE> body = Entries(entries);
  const ULONGLONG entry_offset =
      line.Number("entry_offset", default_block_entry_offset);
  if (block_index_header + entry_offset + body.size() > size) {
    throw std::runtime_error("the entries do not fit in the block: " +
                             line.Source());
  }

  Data::IndexBlock block{};
  block.magic = Data::index_block_magic;
  block.offset_of_us = gsl::narrow<WORD>(block_array_offset);
  block.size_of_us =
      gsl::narrow<WORD>(1 + (size / UpdateSequence::sector_bytes));
  block.lsn = line.Number("lsn", 0);
  block.vcn = line.Number("vcn", 0);
  block.entry_offset = gsl::narrow<DWORD>(entry_offset);
  block.total_entry_size = gsl::narrow<DWORD>(entry_offset + body.size());
  block.alloc_entry_size =
      gsl::narrow<DWORD>(line.Number("alloc", size - block_index_header));
  block.not_leaf = gsl::narrow<BYTE>(line.Number("not_leaf", 0));
  std::memcpy(bytes.data(), &block, sizeof(block));
  std::ranges::copy(
      body, bytes.begin() +
                static_cast<std::ptrdiff_t>(block_index_header + entry_offset));
  UpdateSequence::Protect(bytes, block_array_offset,
                          gsl::narrow<WORD>(line.Number("usn", 1)));
  return bytes;
}

std::vector<BYTE> IndexLayer::Root(const LayerLine& line,
                                   const std::vector<LayerLine>& entries) {
  const std::vector<BYTE> body = Entries(entries);
  Data::IndexRoot root{};
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = gsl::narrow<DWORD>(line.Number("rule", collate_file_name));
  root.ib_size = gsl::narrow<DWORD>(line.Number("block", default_block_bytes));
  root.clusters_per_ib = gsl::narrow<BYTE>(line.Number("clusters", 1));
  root.entry_offset =
      gsl::narrow<DWORD>(sizeof(Data::IndexRoot) - root_index_header);
  root.total_entry_size = gsl::narrow<DWORD>(root.entry_offset + body.size());
  root.alloc_entry_size =
      gsl::narrow<DWORD>(line.Number("alloc", root.total_entry_size));
  root.flags = gsl::narrow<BYTE>(line.Number("children", 0));

  std::vector<BYTE> bytes(sizeof(root) + body.size());
  std::memcpy(bytes.data(), &root, sizeof(root));
  std::ranges::copy(body, bytes.begin() + sizeof(root));
  return bytes;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
