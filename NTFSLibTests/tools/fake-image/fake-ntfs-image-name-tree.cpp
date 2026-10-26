#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename-namespace.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-root.h"
#include "data/ntfs-bpb.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Where the update sequence array of a record of the tree starts, and the
// attributes after it. The array holds the number and one word for each of the
// two sectors, so it ends at 54.
constexpr WORD tree_us_offset = 48;
constexpr WORD tree_attr_offset = 56;

// The update sequence number of every record and index block of the tree.
constexpr WORD tree_usn = 0x7A7B;

// The sequence number of a node's record while it is in use. NTFS gives a
// freed record the next one.
constexpr WORD tree_sequence = 1;

// Where an index block of the tree keeps its update sequence array, and where
// its entries start, as NTFS lays a block out. The index header, whose offsets
// the sizes count from, starts at 0x18.
constexpr size_t block_us_offset = 0x28;
constexpr size_t block_first_entry = 0x40;
constexpr size_t block_index_header = 0x18;

// Size of an index block of the tree: one cluster, so a block's VCN is its
// number.
constexpr DWORD tree_block_size = fake_cluster_size;

// Bytes of a block the entries of one leaf take at most: what is left after the
// header and the end entry.
constexpr size_t block_entry_room = tree_block_size - block_first_entry - 16;

// Index blocks start this many clusters past the last record of the $MFT.
constexpr ULONGLONG blocks_gap_clusters = 1;

// Bytes the end marker of a record's attributes takes.
constexpr size_t end_marker_bytes = 8;

// Bytes of the header of a non-resident $INDEX_ALLOCATION that has one run.
constexpr size_t index_alloc_attr_bytes =
    sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident) + run_list_room;

// The entries of one directory, and where they go.
struct DirectoryPlan {
  // All the entries, in index order.
  std::vector<FakeIndexName> entries;
  // Whether the entries sit in the $INDEX_ROOT. Else they sit in blocks.
  bool resident = true;
  // The entries of each block, and the separators between two blocks.
  std::vector<std::vector<FakeIndexName>> groups;
  std::vector<FakeIndexName> separators;
  // LCN of the first block.
  ULONGLONG first_lcn = 0;
};

// Replaces the last word of each sector of bytes by usn, after saving it in
// the array that starts at us_offset: the on-disk form of a record or a block.
void ApplyFixups(std::span<BYTE> bytes, size_t us_offset, WORD usn) {
  const size_t sectors = bytes.size() / bytes_per_sector;
  std::memcpy(&bytes[us_offset], &usn, sizeof(usn));
  for (size_t sector = 0; sector < sectors; sector++) {
    const size_t last_word = ((sector + 1) * bytes_per_sector) - sizeof(WORD);
    std::memcpy(&bytes[us_offset + sizeof(WORD) * (1 + sector)],
                &bytes[last_word], sizeof(WORD));
    std::memcpy(&bytes[last_word], &usn, sizeof(usn));
  }
}

// An empty record of the tree: its header, with the update sequence array and
// the attributes where the tree puts them.
FakeRecord MakeTreeRecord(NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag flags,
                          WORD sequence) {
  FakeRecord record = MakeRecordHeader(tree_attr_offset, flags, sequence);
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.offset_of_us = tree_us_offset;
    header.size_of_us = 3;
  });
  return record;
}

// Ends the attributes of record at offset, and applies its fixups.
void FinishTreeRecord(FakeRecord& record, DWORD offset) {
  WriteEndOfAttributesMarker(record, offset);
  ApplyFixups(record, tree_us_offset, tree_usn);
}

// The file reference an entry or a $FILE_NAME uses to name the node of index
// node, or the root.
ULONGLONG ReferenceOf(std::size_t node) {
  if (node == name_tree_root) {
    return root_file_reference;
  }
  return MakeFileReference(name_tree_first_record + node, tree_sequence);
}

// Size of the index entry that name makes.
size_t EntrySize(const FakeIndexName& name) {
  std::array<BYTE, tree_block_size> scratch{};
  return WriteFilenameEntry(scratch.data(), name);
}

// The entries of every directory, sorted, the root's last. A freed node has
// none, unless it keeps them: NTFS removes the entries of a record when it
// frees it.
std::vector<DirectoryPlan> CollectEntries(std::span<const FakeTreeNode> nodes,
                                          const FakeNameTreeOptions& options) {
  std::vector<DirectoryPlan> plans(nodes.size() + 1);
  for (std::size_t node = 0; node < nodes.size(); node++) {
    const FakeTreeNode& tree_node = nodes[node];
    if (!tree_node.in_use && !tree_node.keep_entries) {
      continue;
    }
    for (const FakeTreeLink& link : tree_node.links) {
      const std::size_t slot =
          link.parent == name_tree_root ? nodes.size() : link.parent;
      // The parent is not a node of the tree, and not the root, or it is a
      // file.
      if (slot > nodes.size() ||
          (slot < nodes.size() && !nodes[slot].directory)) {
        throw std::invalid_argument("a name is filed under a file");
      }
      plans[slot].entries.push_back(
          {.name = link.name,
           .mft_ref = name_tree_first_record + node,
           .directory = tree_node.directory,
           .sequence = tree_sequence,
           .alter_time = name_tree_write_time,
           .parent_ref = ReferenceOf(link.parent),
           .name_space =
               static_cast<NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace>(
                   link.name_space)});
    }
  }

  // The collation of the volume, then the order of the code units: the order
  // names that are equal once upcased keep, as NTFS keeps them.
  const UpCase::Table& table = UpCase::Table::BuiltIn();
  for (DirectoryPlan& plan : plans) {
    if (!options.collate) {
      continue;
    }
    std::ranges::stable_sort(
        plan.entries,
        [&table](const FakeIndexName& left, const FakeIndexName& right) {
          const int order = table.Compare(left.name, right.name);
          return order != 0 ? order < 0 : left.name < right.name;
        });
  }
  return plans;
}

// Cuts the entries of plan into blocks, leaving the separators between them.
// The last block is never empty.
void SplitIntoBlocks(DirectoryPlan& plan) {
  const std::vector<FakeIndexName>& all = plan.entries;
  std::size_t next = 0;
  while (next < all.size()) {
    std::size_t bytes = 0;
    std::size_t take = 0;
    // The next entry is there, and still fits the block.
    while (next + take < all.size() &&
           bytes + EntrySize(all[next + take]) <= block_entry_room) {
      bytes += EntrySize(all[next + take]);
      take++;
    }
    if (next + take == all.size()) {
      plan.groups.emplace_back(all.begin() + gsl::narrow<std::ptrdiff_t>(next),
                               all.end());
      return;
    }
    // A separator, then at least one more entry for the block after it.
    if (next + take + 1 == all.size()) {
      take--;
    }
    if (take == 0) {
      throw std::length_error("an entry does not fit an index block");
    }
    const auto first = all.begin() + gsl::narrow<std::ptrdiff_t>(next);
    plan.groups.emplace_back(first, first + gsl::narrow<std::ptrdiff_t>(take));
    plan.separators.push_back(all[next + take]);
    next += take + 1;
  }
}

// Bytes the entries of an index root take: the entries, then the end entry.
size_t RootEntriesSize(std::span<const FakeIndexName> names, bool pointers) {
  std::size_t bytes = 0;
  for (const FakeIndexName& name : names) {
    FakeIndexName with_pointer = name;
    if (pointers) {
      with_pointer.sub_node_vcn = 0;
    }
    bytes += EntrySize(with_pointer);
  }
  // The end entry: a bare one, or one that points at the last block.
  std::array<BYTE, tree_block_size> scratch{};
  return bytes + (pointers ? WriteSubNodeEntry(scratch.data(), 0, true)
                           : WriteTerminalEntry(scratch.data()));
}

// Bytes an index root attribute takes with entries_size bytes of entries.
size_t RootAttributeSize(std::size_t entries_size) {
  return AlignAttrSize(sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident) +
                       sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexRoot) +
                       entries_size);
}

// Decides where the entries of a directory go, given the bytes the record
// already uses before its index.
void PlaceEntries(DirectoryPlan& plan, std::size_t used_bytes) {
  const std::size_t resident_need =
      used_bytes + RootAttributeSize(RootEntriesSize(plan.entries, false)) +
      end_marker_bytes;
  if (resident_need <= fake_file_record_size) {
    plan.resident = true;
    return;
  }

  plan.resident = false;
  SplitIntoBlocks(plan);
  const std::size_t need =
      used_bytes + RootAttributeSize(RootEntriesSize(plan.separators, true)) +
      index_alloc_attr_bytes + end_marker_bytes;
  if (need > fake_file_record_size) {
    throw std::length_error(
        "the separators of a directory do not fit its root");
  }
}

// Writes the $INDEX_ROOT of a directory at record[offset], and returns its
// size.
DWORD WriteTreeIndexRoot(FakeRecord& record, DWORD offset,
                         const DirectoryPlan& plan) {
  return WriteResidentIndexRoot(
      record, offset, {}, [&plan](std::span<BYTE> bytes) -> DWORD {
        DWORD size = 0;
        if (plan.resident) {
          for (const FakeIndexName& name : plan.entries) {
            size += WriteFilenameEntry(&bytes[size], name);
          }
          return size + WriteTerminalEntry(&bytes[size]);
        }
        // Each separator points at the block before it, and the end entry at
        // the last one.
        for (std::size_t block = 0; block < plan.separators.size(); block++) {
          FakeIndexName separator = plan.separators[block];
          separator.sub_node_vcn = block;
          size += WriteFilenameEntry(&bytes[size], separator);
        }
        return size +
               WriteSubNodeEntry(&bytes[size], plan.groups.size() - 1, true);
      });
}

// The bytes of the index block that holds names, as it sits on disk.
std::vector<BYTE> MakeLeafBlock(std::span<const FakeIndexName> names,
                                ULONGLONG vcn) {
  std::vector<BYTE> block(tree_block_size, 0);
  std::memcpy(block.data(), "INDX", 4);
  const auto put = [&block](std::size_t offset, auto value) {
    std::memcpy(&block.at(offset), &value, sizeof(value));
  };
  put(4, static_cast<WORD>(block_us_offset));
  put(6, static_cast<WORD>(1 + (tree_block_size / bytes_per_sector)));
  put(16, vcn);

  std::size_t at = block_first_entry;
  for (const FakeIndexName& name : names) {
    at += WriteFilenameEntry(&block.at(at), name);
  }
  at += WriteTerminalEntry(&block.at(at));

  put(0x18, static_cast<DWORD>(block_first_entry - block_index_header));
  put(0x1C, static_cast<DWORD>(at - block_index_header));
  put(0x20, static_cast<DWORD>(tree_block_size - block_index_header));
  ApplyFixups(block, block_us_offset, tree_usn);
  return block;
}

// The record of node, whose index, when it is a directory, plan lays out.
FakeRecord MakeNodeRecord(const FakeTreeNode& node, const DirectoryPlan& plan) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;

  FileRecordFlag flags{};
  if (node.in_use) {
    flags = flags | FileRecordFlag::InUse;
  }
  if (node.directory) {
    flags = flags | FileRecordFlag::Dir;
  }
  FakeRecord record =
      MakeTreeRecord(flags, node.in_use ? tree_sequence
                                        : gsl::narrow<WORD>(tree_sequence + 1));

  DWORD offset = tree_attr_offset;
  offset +=
      WriteStandardInformationAttr(record, offset, StdInfoPermission::Normal);
  for (const FakeTreeLink& link : node.links) {
    offset += WriteFileNameAttr(
        record, offset,
        {.name = link.name,
         .parent_ref = ReferenceOf(link.parent),
         .name_space =
             static_cast<NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace>(
                 link.name_space)},
        node.directory);
  }
  if (!node.directory && !node.runs.empty()) {
    offset += WriteNonResidentAttr(record, offset, Attr::Type::Data,
                                   {.comp_unit_size = 0,
                                    .real_size = node.stream_size,
                                    .runs = node.runs});
  } else if (!node.directory) {
    offset += WriteResidentDataAttr(record, offset, node.data_size);
  } else {
    offset += WriteTreeIndexRoot(record, offset, plan);
    if (!plan.resident) {
      offset += WriteSingleRunIndexAllocAttr(
          record, offset,
          {.start_vcn = 0,
           .clusters = gsl::narrow<DWORD>(plan.groups.size()),
           .lcn = gsl::narrow<DWORD>(plan.first_lcn)},
          ULONGLONG{tree_block_size} * plan.groups.size());
    }
  }
  FinishTreeRecord(record, offset);
  return record;
}

// The bytes a directory record uses before its index: the attributes that
// MakeNodeRecord() writes first.
std::size_t BytesBeforeIndex(const FakeTreeNode& node) {
  FakeRecord scratch = MakeTreeRecord({}, tree_sequence);
  DWORD offset = tree_attr_offset;
  offset += WriteStandardInformationAttr(
      scratch, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  for (const FakeTreeLink& link : node.links) {
    offset += WriteFileNameAttr(
        scratch, offset, {.name = link.name, .parent_ref = root_file_reference},
        true);
  }
  return offset;
}

// The record of the $MFT: one run over every slot.
FakeRecord MakeNameTreeMftRecord(ULONGLONG record_count) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;

  FakeRecord record = MakeTreeRecord(FileRecordFlag::InUse, 1);
  DWORD offset = tree_attr_offset;
  offset +=
      WriteStandardInformationAttr(record, offset, StdInfoPermission::Normal);
  offset += WriteFileNameAttr(
      record, offset, {.name = L"$MFT", .parent_ref = root_file_reference},
      false);

  // One run of record_count clusters at the first cluster after the boot
  // sector, with a four byte length: the encoding of a fixture run takes one.
  std::vector<BYTE> raw_runs{0x44};
  const auto count = gsl::narrow<DWORD>(record_count);
  const auto first_lcn = gsl::narrow<DWORD>(mft_lcn);
  raw_runs.resize(1 + sizeof(count) + sizeof(first_lcn) + 1, 0);
  std::memcpy(&raw_runs.at(1), &count, sizeof(count));
  std::memcpy(&raw_runs.at(1 + sizeof(count)), &first_lcn, sizeof(first_lcn));
  offset +=
      WriteNonResidentAttr(record, offset, Attr::Type::Data,
                           {.comp_unit_size = 0,
                            .real_size = record_count * fake_file_record_size,
                            .runs = {{.lcn = mft_lcn, .clusters = count}},
                            .overrides = {.raw_runs = raw_runs}});
  FinishTreeRecord(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithNameTree(std::span<const FakeTreeNode> nodes,
                                   const FakeNameTreeOptions& options) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  const ULONGLONG record_count = name_tree_first_record + nodes.size();
  if (record_count > (std::numeric_limits<DWORD>::max)()) {
    throw std::length_error("too many nodes");
  }

  std::vector<DirectoryPlan> plans = CollectEntries(nodes, options);
  // The index blocks lie after the $MFT, in the order of the directories.
  ULONGLONG next_lcn = mft_lcn + record_count + blocks_gap_clusters;
  for (std::size_t node = 0; node <= nodes.size(); node++) {
    DirectoryPlan& plan = plans[node];
    if (node < nodes.size() && !nodes[node].directory) {
      continue;
    }
    const std::size_t used =
        node < nodes.size()
            ? BytesBeforeIndex(nodes[node])
            : BytesBeforeIndex({.links = {{.name = L"."}}, .directory = true});
    PlaceEntries(plan, used);
    if (!plan.resident) {
      plan.first_lcn = next_lcn;
      next_lcn += plan.groups.size();
    }
  }

  std::vector<BYTE> image = BuildFakeNtfsImage();
  PadToFullCacheRead(image,
                     mft_address + (record_count * fake_file_record_size));
  PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeNameTreeMftRecord(record_count));

  // The root has the name every volume gives it, ".".
  FakeRecord root = MakeNodeRecord(
      {.links = {{.name = L"."}}, .directory = true}, plans.back());
  EditFileRecordHeader(root, [](FileRecordHeader& header) {
    header.seq_no = root_sequence_number;
  });
  PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Root), root);

  for (std::size_t node = 0; node < nodes.size(); node++) {
    PutMftRecord(image, name_tree_first_record + node,
                 MakeNodeRecord(nodes[node], plans[node]));
  }

  // FullCache reads whole 64 KiB blocks: the image runs past the last block.
  PadToFullCacheRead(image, (next_lcn + 1) * fake_cluster_size);
  for (const DirectoryPlan& plan : plans) {
    for (std::size_t block = 0; block < plan.groups.size(); block++) {
      const std::vector<BYTE> bytes = MakeLeafBlock(plan.groups[block], block);
      std::ranges::copy(bytes, image.begin() + gsl::narrow<std::ptrdiff_t>(
                                                   (plan.first_lcn + block) *
                                                   fake_cluster_size));
    }
  }

  // The volume spans the whole image, so every record and block sits inside it.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.total_sectors = image.size() / bytes_per_sector;
  return image;
}

std::vector<FakeTreeNode>
    MakeNameChain(std::size_t depth,
                  const std::function<std::wstring(std::size_t)>& name_of,
                  std::wstring leaf_name) {
  std::vector<FakeTreeNode> nodes;
  nodes.reserve(depth + 1);
  for (std::size_t level = 0; level < depth; level++) {
    nodes.push_back(
        {.links = {{.name = name_of(level),
                    .parent = level == 0 ? name_tree_root : level - 1}},
         .directory = true});
  }
  nodes.push_back(
      {.links = {{.name = std::move(leaf_name),
                  .parent = depth == 0 ? name_tree_root : depth - 1}}});
  return nodes;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
