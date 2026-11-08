#include "index-edit.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "record-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The fixed part of an index entry, before its stream, and the size of the
// VCN a sub-node entry ends with.
constexpr std::size_t entry_header_bytes = 16;
constexpr std::size_t sub_node_vcn_bytes = 8;

// The index entries are counted from offset 0x18 of an index block, after the
// fixed header. A record or a block of one sector keeps its update sequence
// number in its last word, so the entries stop before it.
constexpr std::size_t block_header_bytes = 0x18;
constexpr std::size_t last_word_offset = 0x3FE;

}  // namespace

void IndexEdit::Remove(std::vector<BYTE>& image, std::size_t offset,
                       std::size_t count, std::size_t limit) {
  static_cast<void>(image.at(limit - 1));
  const auto first = image.begin();
  std::copy(first + static_cast<std::ptrdiff_t>(offset + count),
            first + static_cast<std::ptrdiff_t>(limit),
            first + static_cast<std::ptrdiff_t>(offset));
  std::fill(first + static_cast<std::ptrdiff_t>(limit - count),
            first + static_cast<std::ptrdiff_t>(limit), BYTE{0});
}

std::size_t IndexEdit::FindBlock(const std::vector<BYTE>& image) {
  constexpr std::array<BYTE, 4> magic = {'I', 'N', 'D', 'X'};
  const auto found =
      std::search(image.begin(), image.end(), magic.begin(), magic.end());
  if (found == image.end()) {
    throw std::out_of_range("no index block in the image");
  }
  return static_cast<std::size_t>(found - image.begin());
}

void IndexEdit::UnpadRoot(std::vector<BYTE>& image, ULONGLONG idx) {
  const std::size_t record = RecordEdit::RecordOffset(idx);
  const std::size_t attr =
      RecordEdit::Attribute(image, idx, RecordEdit::type_index_root);
  const DWORD removed =
      Unpad(image, RecordEdit::FirstIndexRootEntry(image, attr),
            record + last_word_offset);
  RecordEdit::Edit<Data::IndexRoot>(image, RecordEdit::ValueOffset(image, attr),
                                    [&](auto& header) {
                                      header.total_entry_size -= removed;
                                      header.alloc_entry_size -= removed;
                                    });
  RecordEdit::Edit<Data::HeaderResident>(image, attr, [&](auto& header) {
    header.attr_size -= removed;
    header.header.total_size -= removed;
  });
}

void IndexEdit::UnpadBlock(std::vector<BYTE>& image, std::size_t offset) {
  DWORD entries = 0;
  RecordEdit::Edit<Data::IndexBlock>(
      image, offset, [&](auto& block) { entries = block.entry_offset; });
  const DWORD removed = Unpad(image, offset + block_header_bytes + entries,
                              offset + last_word_offset);
  RecordEdit::Edit<Data::IndexBlock>(image, offset, [&](auto& block) {
    block.total_entry_size -= removed;
    block.alloc_entry_size -= removed;
  });
}

DWORD IndexEdit::Unpad(std::vector<BYTE>& image, std::size_t first,
                       std::size_t limit) {
  DWORD removed = 0;
  std::size_t at = first;
  for (;;) {
    std::size_t padding = 0;
    std::size_t stream_end = 0;
    bool last = false;
    RecordEdit::Edit<Data::IndexEntry>(image, at, [&](auto& entry) {
      const bool sub_node = (entry.flags & Data::IndexEntryFlag::SubNode) !=
                            Data::IndexEntryFlag::None;
      last = (entry.flags & Data::IndexEntryFlag::Last) !=
             Data::IndexEntryFlag::None;
      const std::size_t used = entry_header_bytes + entry.stream_size +
                               (sub_node ? sub_node_vcn_bytes : 0);
      padding = entry.size > used ? entry.size - used : 0;
      stream_end = at + entry_header_bytes + entry.stream_size;
      entry.size = static_cast<WORD>(entry.size - padding);
    });
    if (padding != 0) {
      Remove(image, stream_end, padding, limit);
      removed += static_cast<DWORD>(padding);
    }
    if (last) {
      return removed;
    }
    RecordEdit::Edit<Data::IndexEntry>(image, at,
                                       [&](auto& entry) { at += entry.size; });
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
