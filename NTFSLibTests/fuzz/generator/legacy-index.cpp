#include "legacy-index.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// The fixed part of an index entry, of an $INDEX_ROOT value (the root header
// and the index header), and of a $FILE_NAME before its name.
constexpr std::size_t entry_header_bytes = 16;
constexpr std::size_t root_header_bytes = 32;
constexpr std::size_t file_name_header_bytes =
    offsetof(NTFS_BROWSER_NAMESPACE::Data::Filename, name);

// The entries of an index start on a multiple of 8 bytes.
constexpr std::size_t alignment = 8;

// The directory that holds every name: the root, with the sequence number 5.
constexpr ULONGLONG parent_record = 5;
constexpr unsigned sequence_shift = 48;
constexpr ULONGLONG parent_sequence = 5;

// The times of every name: a base in 2015 for the creation, then one more for
// each of the next three. The sizes are 16 KiB, and the file is archived.
constexpr ULONGLONG time_base = 0x01D0ULL << sequence_shift;
constexpr ULONGLONG name_size = 0x4000;
constexpr DWORD archive_flag = 0x20;

// The Win32 name space.
constexpr BYTE win32_name_space = 1;

// The collation rule of file names, and the clusters of an index block.
constexpr DWORD file_name_collation = 1;
constexpr DWORD block_bytes = FakeImage::fake_cluster_size;
constexpr BYTE block_clusters = 1;

// An index block keeps its array at 0x28 (the number, and one word for each of
// the two sectors) and its first entry after it. The sizes of the block count
// from its index header at 0x18.
constexpr WORD block_array_offset = 0x28;
constexpr WORD block_array_words = 3;
constexpr DWORD block_index_header = 0x18;
constexpr DWORD block_first_entry = 0x30;

std::size_t Align(std::size_t value) {
  return (value + alignment - 1) / alignment * alignment;
}

template <typename T>
void Put(std::vector<BYTE>& out, T value) {
  const std::size_t at = out.size();
  out.resize(at + sizeof(T));
  std::memcpy(&out[at], &value, sizeof(T));
}

}  // namespace

std::vector<BYTE> LegacyIndex::FileNameValue(std::u16string_view name) {
  std::vector<BYTE> value(file_name_header_bytes + (name.size() * sizeof(WORD)),
                          0);
  NTFS_BROWSER_NAMESPACE::Data::Filename header{};
  header.parent_ref = parent_record | (parent_sequence << sequence_shift);
  header.create_time = time_base;
  header.alter_time = time_base + 1;
  header.mft_time = time_base + 2;
  header.read_time = time_base + 3;
  header.alloc_size = name_size;
  header.real_size = name_size;
  header.flags =
      static_cast<NTFS_BROWSER_NAMESPACE::Data::FilenameFlag>(archive_flag);
  header.name_length = static_cast<BYTE>(name.size());
  header.name_space =
      static_cast<NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace>(
          win32_name_space);
  std::memcpy(value.data(), &header, file_name_header_bytes);
  for (std::size_t i = 0; i < name.size(); ++i) {
    const auto unit = static_cast<WORD>(name[i]);
    std::memcpy(&value[file_name_header_bytes + (i * sizeof(WORD))], &unit,
                sizeof(WORD));
  }
  return value;
}

std::vector<BYTE> LegacyIndex::Entry(ULONGLONG record,
                                     std::u16string_view name) {
  const std::vector<BYTE> value = FileNameValue(name);
  std::vector<BYTE> entry(Align(entry_header_bytes + value.size()), 0);
  NTFS_BROWSER_NAMESPACE::Data::IndexEntry header{};
  header.mft_index = record;
  header.size = static_cast<WORD>(entry.size());
  header.stream_size = static_cast<WORD>(value.size());
  // Only the fixed part of the entry is the header: the stream follows it.
  std::memcpy(entry.data(), &header, entry_header_bytes);
  std::ranges::copy(value, entry.begin() + entry_header_bytes);
  return entry;
}

std::vector<BYTE> LegacyIndex::LastEntry(bool sub_node,
                                         std::optional<ULONGLONG> vcn) {
  std::vector<BYTE> entry(entry_header_bytes, 0);
  NTFS_BROWSER_NAMESPACE::Data::IndexEntry header{};
  header.size =
      static_cast<WORD>(entry_header_bytes + (vcn ? sizeof(*vcn) : 0));
  header.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  if (sub_node) {
    header.flags |= NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode;
  }
  std::memcpy(entry.data(), &header, entry_header_bytes);
  if (vcn) {
    Put<ULONGLONG>(entry, *vcn);
  }
  return entry;
}

std::vector<BYTE> LegacyIndex::RootValue(const std::vector<BYTE>& entries,
                                         bool children) {
  NTFS_BROWSER_NAMESPACE::Data::IndexRoot header{};
  header.attr_type = static_cast<NTFS_BROWSER_NAMESPACE::Attr::Type>(0x30);
  header.coll_rule = file_name_collation;
  header.ib_size = block_bytes;
  header.clusters_per_ib = block_clusters;
  header.entry_offset = entry_header_bytes;
  header.total_entry_size =
      static_cast<DWORD>(entry_header_bytes + entries.size());
  header.alloc_entry_size = header.total_entry_size;
  header.flags = children ? 1 : 0;
  std::vector<BYTE> value(root_header_bytes, 0);
  std::memcpy(value.data(), &header, root_header_bytes);
  value.insert(value.end(), entries.begin(), entries.end());
  return value;
}

std::vector<BYTE> LegacyIndex::Block(ULONGLONG vcn,
                                     const std::vector<BYTE>& entries) {
  std::vector<BYTE> block(block_bytes, 0);
  NTFS_BROWSER_NAMESPACE::Data::IndexBlock header{};
  header.magic = NTFS_BROWSER_NAMESPACE::Data::index_block_magic;
  header.offset_of_us = block_array_offset;
  header.size_of_us = block_array_words;
  header.vcn = vcn;
  header.entry_offset = block_first_entry - block_index_header;
  header.total_entry_size = static_cast<DWORD>(
      block_first_entry - block_index_header + entries.size());
  header.alloc_entry_size = block_bytes - block_index_header;
  std::memcpy(block.data(), &header, sizeof(header));
  std::ranges::copy(entries, block.begin() + block_first_entry);
  return block;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
