#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/namespace.h>

#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

WORD WriteFilenameEntry(BYTE* dest, const FakeIndexName& name) {
  auto& entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(dest);
  entry.mft_index = name.mft_ref;
  entry.mft_sn = name.sequence;

  auto& filename =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(&entry.stream);
  filename.parent_ref = name.parent_ref;
  filename.alter_time = name.alter_time;
  filename.alloc_size = name.alloc_size;
  filename.real_size = name.real_size;
  filename.flags = name.directory
                       ? NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::Directory
                       : NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::None;
  const std::vector<WORD> encoded_name = ToUtf16(name.name);
  filename.name_length = gsl::narrow<BYTE>(encoded_name.size());
  filename.name_space = NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace::Win32;
  for (size_t i = 0; i < encoded_name.size(); i++) {
    filename.name[i] = encoded_name.at(i);
  }

  entry.stream_size = gsl::narrow<WORD>(
      reinterpret_cast<BYTE*>(&filename.name[filename.name_length]) -
      reinterpret_cast<BYTE*>(&filename));
  auto flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag{};
  if (name.last) {
    flags = flags | NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  }
  if (name.sub_node_vcn) {
    flags = flags | NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode;
  }
  entry.flags = flags;
  // The sub-node VCN, when there is one, follows the stream.
  entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &entry.stream - reinterpret_cast<BYTE*>(&entry) + entry.stream_size +
      (name.sub_node_vcn ? sizeof(ULONGLONG) : 0)));
  if (name.sub_node_vcn) {
    SubNodeVcnSlot(entry) = *name.sub_node_vcn;
  }
  return entry.size;
}

WORD WriteTerminalEntry(BYTE* dest) {
  auto& entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(dest);
  entry.flags = NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last;
  entry.stream_size = 0;
  entry.size = gsl::narrow<WORD>(
      AlignAttrSize(&entry.stream - reinterpret_cast<BYTE*>(&entry)));
  return entry.size;
}

WORD WriteSubNodeEntry(BYTE* dest, ULONGLONG vcn, bool last) {
  auto& entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexEntry*>(dest);
  entry.mft_index = 0;
  entry.mft_sn = 0;
  entry.stream_size = 0;
  entry.flags = last ? NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode |
                           NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::Last
                     : NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag::SubNode;
  entry.size = gsl::narrow<WORD>(
      AlignAttrSize(offsetof(NTFS_BROWSER_NAMESPACE::Data::IndexEntry, stream) +
                    sizeof(ULONGLONG)));
  SubNodeVcnSlot(entry) = vcn;
  return entry.size;
}

DWORD WriteResidentIndexRoot(FakeRecord& record, DWORD offset,
                             const FakeIndexBlockGeometry& geometry,
                             const IndexEntriesWriter& write_entries) {
  auto& attr = InitResidentAttr(record, offset, Attr::Type::IndexRoot);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + attr.attr_offset);
  auto& root =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = geometry.ib_size;
  root.clusters_per_ib = geometry.clusters_per_ib;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const DWORD entries_size = write_entries(
      body.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)));
  root.total_entry_size = entries_size;
  root.alloc_entry_size = entries_size;
  root.flags = 0;

  attr.attr_size =
      static_cast<DWORD>(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot)) +
      entries_size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
  return attr.header.total_size;
}

DWORD WriteSubNodeIndexRootAttr(FakeRecord& record, DWORD offset, DWORD ib_size,
                                BYTE clusters_per_ib) {
  return WriteResidentIndexRoot(
      record, offset, {.ib_size = ib_size, .clusters_per_ib = clusters_per_ib},
      [](std::span<BYTE> entries) -> DWORD {
        return WriteSubNodeEntry(entries.data(), 0, true);
      });
}

DWORD WriteIndexRootAttr(FakeRecord& record, DWORD offset,
                         std::span<const FakeIndexName> root_names) {
  return WriteResidentIndexRoot(
      record, offset, {}, [root_names](std::span<BYTE> entries) -> DWORD {
        DWORD leaf_bytes = 0;
        for (const FakeIndexName& name : root_names) {
          leaf_bytes += WriteFilenameEntry(&gsl::at(entries, leaf_bytes), name);
        }
        return leaf_bytes +
               WriteSubNodeEntry(&gsl::at(entries, leaf_bytes), 0, true);
      });
}

DWORD WriteFileNameIndexRootAttr(FakeRecord& record, DWORD offset,
                                 std::wstring_view name, ULONGLONG mft_index,
                                 ULONGLONG parent_ref) {
  return WriteResidentIndexRoot(
      record, offset, {}, [&](std::span<BYTE> entries) -> DWORD {
        const WORD first_size =
            WriteFilenameEntry(entries.data(), {.name = name,
                                                .mft_ref = mft_index,
                                                .directory = false,
                                                .parent_ref = parent_ref});
        return first_size + WriteTerminalEntry(&gsl::at(entries, first_size));
      });
}

NTFS_BROWSER_NAMESPACE::Data::IndexBlock&
    WriteIndexBlockHeader(std::span<BYTE> block_start, DWORD block_size,
                          ULONGLONG vcn) {
  auto& block = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::IndexBlock*>(
      block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points the USA at the block's own last (1 + sectors) words, so every
  // PatchUS() check compares a still-zero byte range to itself and the whole
  // array trivially self-patches, without a real fixup array.
  const DWORD sectors = block_size / bytes_per_sector;
  block.offset_of_us = gsl::narrow<WORD>(block_size - 2 * (1 + sectors));
  block.size_of_us = gsl::narrow<WORD>(1 + sectors);
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start,
                sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;
  return block;
}

void WriteIndexLeafBlockAt(std::vector<BYTE>& image,
                           const FakeIndexLeafBlock& spec) {
  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(spec.offset);
  auto& block = WriteIndexBlockHeader(block_start, spec.size, spec.vcn);

  const WORD entry_size = WriteFilenameEntry(
      &gsl::at(block_start, sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock)),
      {.name = spec.name,
       .mft_ref = spec.mft_ref,
       .directory = false,
       .parent_ref = spec.parent_ref,
       .last = true});
  block.total_entry_size = entry_size;
  block.alloc_entry_size = entry_size;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
