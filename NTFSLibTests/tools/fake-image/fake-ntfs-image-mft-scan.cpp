#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gsl/util>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/attribute-list.h"
#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/ntfs-bpb.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// LCN of the second fragment. Far from the first one, which sits at mft_lcn,
// so that two fragments are never contiguous.
constexpr ULONGLONG later_fragments_lcn = 1000;

// Clusters left unused after each of the later fragments.
constexpr ULONGLONG fragment_gap_clusters = 50;

// Offset of the last word of a record's first sector, where the update
// sequence number sits.
constexpr size_t first_sector_end_offset = 510;

// A value the update sequence number (0 in these fixtures) never has.
constexpr BYTE wrong_update_sequence_byte = 0xAB;

// How many kinds of slot the fixture cycles through.
constexpr ULONGLONG slot_kind_count = 8;

// LCN of fragment number fragment.
ULONGLONG FragmentLcn(ULONGLONG fragment, ULONGLONG fragment_clusters) {
  if (fragment == 0) {
    return mft_lcn;
  }
  return later_fragments_lcn +
         (fragment - 1) * (fragment_clusters + fragment_gap_clusters);
}

// The $MFT record: its $DATA maps record_count clusters (one per record) in
// fragments of fragment_clusters.
FakeRecord MakeScanMftRecord(ULONGLONG record_count,
                             ULONGLONG fragment_clusters) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  EditFileRecordHeader(record,
                       [](FileRecordHeader& header) { header.seq_no = 1; });

  std::vector<FakeDataRun> runs;
  for (ULONGLONG first = 0, fragment = 0; first < record_count;
       first += fragment_clusters, fragment++) {
    const ULONGLONG clusters =
        (std::min)(fragment_clusters, record_count - first);
    runs.push_back({.lcn = FragmentLcn(fragment, fragment_clusters),
                    .clusters = static_cast<DWORD>(clusters)});
  }

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(record, offset,
                              {.name = L"$MFT", .parent_ref = root}, false);
  offset +=
      WriteNonResidentAttr(record, offset, Attr::Type::Data,
                           {.comp_unit_size = 0,
                            .real_size = record_count * fake_file_record_size,
                            .runs = runs});
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// The record of slot idx, past the system files. All kinds of slot a scan
// meets come up in turn.
FakeRecord MakeScanSlotRecord(ULONGLONG idx) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const std::wstring name = L"f" + std::to_wstring(idx);
  const FileRecordFlag file = FileRecordFlag::InUse;

  switch (idx % slot_kind_count) {
    case 2:
      // Freed: its sequence number was bumped on deletion.
      return MakeMftTreeRecord(FileRecordFlag{}, 2,
                               {{.name = name, .parent_ref = root}});
    case 3:
      return MakeMftTreeRecord(file | FileRecordFlag::Dir, 1,
                               {{.name = name, .parent_ref = root}});
    case 4:
      // A zero-filled slot, as one NTFS never used.
      return FakeRecord{};
    case 5:
      // An extension of slot 17 (a file, whatever the kind cycle).
      return MakeMftTreeRecord(file, 1, {{.name = name, .parent_ref = root}},
                               {}, StdInfoPermission::Normal,
                               MakeFileReference(17, 1));
    case 6: {
      FakeRecord record =
          MakeMftTreeRecord(file, 1, {{.name = name, .parent_ref = root}},
                            static_cast<DWORD>(idx % 100));
      // The sector no longer ends with the update sequence number.
      record.at(first_sector_end_offset) = wrong_update_sequence_byte;
      return record;
    }
    default:
      return MakeMftTreeRecord(file, 1, {{.name = name, .parent_ref = root}},
                               static_cast<DWORD>(idx % 100));
  }
}

// Sequence numbers of the records that carry an $ATTRIBUTE_LIST in the busy
// fixture, and of the extension record their list names.
constexpr WORD busy_list_base_sequence = 4;
constexpr WORD busy_list_extension_sequence = 2;

// How many kinds of slot the busy fixture cycles through.
constexpr ULONGLONG busy_slot_kind_count = 10;

// Bytes of the $LOGGED_UTILITY_STREAM the busy fixture puts in some files.
constexpr DWORD busy_logged_stream_size = 4;

// A file in use with a resident $LOGGED_UTILITY_STREAM, as a file touched by a
// transaction has: its key stream may have to be read from the volume.
FakeRecord MakeBusyLoggedStreamRecord(ULONGLONG idx) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value,
                       NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse, 1);
  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(
      record, offset, {.name = L"t" + std::to_wstring(idx), .parent_ref = root},
      false);
  offset +=
      WriteResidentDataAttr(record, offset, static_cast<DWORD>(idx % 100));
  auto& stream =
      InitResidentAttr(record, offset, Attr::Type::LoggedUtilityStream);
  stream.attr_size = busy_logged_stream_size;
  stream.header.total_size = AlignAttrSize(sizeof(stream) + stream.attr_size);
  offset += stream.header.total_size;
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// A file in use whose only $FILE_NAME sits in the next record, the extension
// record its resident $ATTRIBUTE_LIST names.
FakeRecord MakeBusyListBaseRecord(ULONGLONG idx) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse,
      busy_list_base_sequence);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);

  NTFS_BROWSER_NAMESPACE::Data::AttributeList entry{};
  entry.attr_type = Attr::Type::FileName;
  entry.record_size = static_cast<WORD>(AlignAttrSize(
      NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size));
  entry.base_ref.segment_number = idx + 1;
  entry.base_ref.sequence_number = busy_list_extension_sequence;
  auto& list = InitResidentAttr(record, offset, Attr::Type::AttributeList);
  list.attr_size = entry.record_size;
  list.header.total_size = AlignAttrSize(sizeof(list) + list.attr_size);
  std::memcpy(&record.at(offset + list.attr_offset), &entry,
              NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size);
  offset += list.header.total_size;
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// The record of slot idx, past the system files, in the busy fixture: the
// kinds of MakeScanSlotRecord(), plus a file with a $LOGGED_UTILITY_STREAM, and
// a file with an $ATTRIBUTE_LIST and its extension record.
FakeRecord MakeBusySlotRecord(ULONGLONG idx) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const std::wstring name = L"f" + std::to_wstring(idx);
  const FileRecordFlag file = FileRecordFlag::InUse;

  switch (idx % busy_slot_kind_count) {
    case 1:
      return MakeBusyLoggedStreamRecord(idx);
    case 2:
    case 9:
      // Freed: its sequence number was bumped on deletion.
      return MakeMftTreeRecord(FileRecordFlag{}, 2,
                               {{.name = name, .parent_ref = root}},
                               static_cast<DWORD>(idx % 50));
    case 3:
      return MakeMftTreeRecord(file | FileRecordFlag::Dir, 1,
                               {{.name = name, .parent_ref = root}});
    case 4:
      // A zero-filled slot, as one NTFS never used.
      return FakeRecord{};
    case 6:
      return MakeBusyListBaseRecord(idx);
    case 7:
      // The extension record of slot idx - 1, which holds its name.
      return MakeMftTreeRecord(
          file, busy_list_extension_sequence,
          {{.name = L"listed" + std::to_wstring(idx - 1), .parent_ref = root}},
          {}, StdInfoPermission::Normal,
          MakeFileReference(idx - 1, busy_list_base_sequence));
    case 8: {
      FakeRecord record =
          MakeMftTreeRecord(file, 1, {{.name = name, .parent_ref = root}},
                            static_cast<DWORD>(idx % 100));
      // The sector no longer ends with the update sequence number.
      record.at(first_sector_end_offset) = wrong_update_sequence_byte;
      return record;
    }
    default:
      return MakeMftTreeRecord(file, 1, {{.name = name, .parent_ref = root}},
                               static_cast<DWORD>(idx % 100));
  }
}

// How many kinds of slot the list scan fixture cycles through.
constexpr ULONGLONG list_scan_kind_count = 14;

// Sequence numbers of the list scan fixture: the files with a list, the
// extension records of a live file, and those of a freed one, which NTFS
// bumped on deletion.
constexpr WORD list_scan_base_sequence = 4;
constexpr WORD list_scan_extension_sequence = 2;
constexpr WORD list_scan_freed_extension_sequence = 3;

// One entry of an $ATTRIBUTE_LIST of the list scan fixture: it names the
// $FILE_NAME of record, under the sequence of a live extension record.
struct ListScanEntry {
  ULONGLONG record;
  WORD sequence;
};

// The bytes of an $ATTRIBUTE_LIST made of entry.
std::vector<BYTE> ListScanBytes(const ListScanEntry& entry) {
  NTFS_BROWSER_NAMESPACE::Data::AttributeList raw{};
  raw.attr_type = Attr::Type::FileName;
  raw.record_size = static_cast<WORD>(AlignAttrSize(
      NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size));
  raw.base_ref.segment_number = entry.record;
  raw.base_ref.sequence_number = entry.sequence;
  std::vector<BYTE> bytes(raw.record_size, 0);
  std::memcpy(bytes.data(), &raw,
              NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size);
  return bytes;
}

// A file with only a $STANDARD_INFORMATION and an $ATTRIBUTE_LIST of list,
// resident, or non-resident in run.
FakeRecord MakeListScanBase(NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag flags,
                            const std::vector<BYTE>& list,
                            const std::optional<FakeDataRun>& run) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, flags, list_scan_base_sequence);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  if (run) {
    offset += WriteNonResidentAttr(
        record, offset, Attr::Type::AttributeList,
        {.comp_unit_size = 0, .real_size = list.size(), .runs = {*run}});
  } else {
    auto& attr = InitResidentAttr(record, offset, Attr::Type::AttributeList);
    attr.attr_size = static_cast<DWORD>(list.size());
    attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
    std::memcpy(&record.at(offset + attr.attr_offset), list.data(),
                list.size());
    offset += attr.header.total_size;
  }
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// The extension record of file base, which holds its name.
FakeRecord
    MakeListScanExtension(NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag flags,
                          WORD sequence, ULONGLONG idx, ULONGLONG base) {
  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  return MakeMftTreeRecord(
      flags, sequence,
      {{.name = L"listed" + std::to_wstring(idx), .parent_ref = root}}, {},
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal,
      MakeFileReference(base, list_scan_base_sequence));
}

// Writes record, the content of $MFT slot idx, in the fragment that holds it.
void PutScanRecord(std::vector<BYTE>& image, ULONGLONG idx,
                   ULONGLONG fragment_clusters, const FakeRecord& record) {
  PutRecordAt(
      image,
      static_cast<size_t>(FragmentedMftScanSlotOffset(idx, fragment_clusters)),
      record);
}

// Multiplier that scatters slots over names. Coprime with
// indexed_scan_name_modulus, so two slots never share a name.
constexpr ULONGLONG indexed_scan_name_stride = 37;

// Number of distinct name numbers: four decimal digits.
constexpr ULONGLONG indexed_scan_name_modulus = 10000;

// Digits of a name number, zero-padded so names sort as numbers.
constexpr size_t indexed_scan_name_digits = 4;

// Size in bytes of an index block of the indexed scan fixture, and the clusters
// it spans. Four clusters of 1024 bytes: a block holds a few dozen names.
constexpr DWORD indexed_scan_block_size = 4096;
constexpr BYTE indexed_scan_block_clusters = 4;

// LCN of the first index block of the indexed scan fixture. Past every
// fragment of the $MFT the fixture maps.
constexpr DWORD indexed_scan_blocks_lcn = 3000;

// The index entry of a slot of the indexed scan fixture, and what it names.
struct IndexedScanEntry {
  std::wstring name;
  FakeIndexName fake;
};

// Name of the entry that names slot idx. The number scatters the slots: names
// in collation order name slots in no order.
std::wstring IndexedScanName(ULONGLONG idx) {
  std::wstring digits = std::to_wstring((idx * indexed_scan_name_stride) %
                                        indexed_scan_name_modulus);
  return L"n" + std::wstring(indexed_scan_name_digits - digits.size(), L'0') +
         digits;
}

// The entries of the indexed scan fixture, in collation order. Every slot past
// the system files has an entry. Some also have an alias entry, and some a
// stale one that names another sequence number.
std::vector<IndexedScanEntry> IndexedScanEntries(ULONGLONG record_count) {
  std::vector<IndexedScanEntry> entries;
  for (ULONGLONG idx = static_cast<ULONGLONG>(Mft::Idx::User);
       idx < record_count; idx++) {
    const bool directory = idx % slot_kind_count == 3;
    entries.push_back(
        {IndexedScanName(idx), {.mft_ref = idx, .directory = directory}});
    if (idx % indexed_scan_alias_every == 0) {
      entries.push_back({L"a" + std::to_wstring(idx),
                         {.mft_ref = idx, .directory = directory}});
    }
    if (idx % indexed_scan_stale_every == 0) {
      entries.push_back({L"r" + std::to_wstring(idx),
                         {.mft_ref = idx,
                          .directory = directory,
                          .sequence = indexed_scan_stale_sequence}});
    }
  }
  std::ranges::sort(entries, {}, &IndexedScanEntry::name);
  for (IndexedScanEntry& entry : entries) {
    entry.fake.name = entry.name;
  }
  return entries;
}

// Writes the entries of one node, each as a name, and the terminating entry
// after them. A separator carries the VCN of the leaf before it.
DWORD WriteIndexedScanNode(std::span<BYTE> dest,
                           std::span<const FakeIndexName> names,
                           std::optional<ULONGLONG> last_vcn) {
  DWORD size = 0;
  for (const FakeIndexName& name : names) {
    size += WriteFilenameEntry(&gsl::at(dest, size), name);
  }
  if (last_vcn) {
    return size + WriteSubNodeEntry(&gsl::at(dest, size), *last_vcn, true);
  }
  return size + WriteTerminalEntry(&gsl::at(dest, size));
}

// The update sequence number a block of the indexed scan fixture carries.
constexpr WORD indexed_scan_update_sequence = 1;

// Gives block a real update sequence array: the last word of each sector moves
// into the array, and the update sequence number takes its place. A block of
// entries needs it, since entries cross sector ends.
void ApplyIndexBlockFixup(std::span<BYTE> block) {
  const auto& header =
      *reinterpret_cast<const NTFS_BROWSER_NAMESPACE::Data::IndexBlock*>(
          block.data());
  const size_t sectors = block.size() / bytes_per_sector;
  std::memcpy(&gsl::at(block, header.offset_of_us),
              &indexed_scan_update_sequence, sizeof(WORD));
  for (size_t sector = 0; sector < sectors; sector++) {
    const size_t end = (sector + 1) * bytes_per_sector - sizeof(WORD);
    std::memcpy(
        &gsl::at(block, header.offset_of_us + (sector + 1) * sizeof(WORD)),
        &gsl::at(block, end), sizeof(WORD));
    std::memcpy(&gsl::at(block, end), &indexed_scan_update_sequence,
                sizeof(WORD));
  }
}

}  // namespace

ULONGLONG FragmentedMftScanSlotOffset(ULONGLONG idx,
                                      ULONGLONG fragment_clusters) {
  const ULONGLONG fragment = idx / fragment_clusters;
  const ULONGLONG lcn =
      FragmentLcn(fragment, fragment_clusters) + idx % fragment_clusters;
  return lcn * cluster_size;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedMftScan(ULONGLONG record_count,
                                            ULONGLONG fragment_clusters) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  PutScanRecord(image, static_cast<ULONGLONG>(Mft::Idx::Root),
                fragment_clusters,
                MakeMftTreeRecord(FileRecordFlag::InUse | FileRecordFlag::Dir,
                                  root_sequence_number,
                                  {{.name = L".", .parent_ref = root}}));
  for (ULONGLONG idx = static_cast<ULONGLONG>(Mft::Idx::User);
       idx < record_count; idx++) {
    PutScanRecord(image, idx, fragment_clusters, MakeScanSlotRecord(idx));
  }
  PutScanRecord(image, static_cast<ULONGLONG>(Mft::Idx::Mft), fragment_clusters,
                MakeScanMftRecord(record_count, fragment_clusters));
  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithBusyMftScan(ULONGLONG record_count,
                                      ULONGLONG fragment_clusters) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  PutScanRecord(image, static_cast<ULONGLONG>(Mft::Idx::Root),
                fragment_clusters,
                MakeMftTreeRecord(FileRecordFlag::InUse | FileRecordFlag::Dir,
                                  root_sequence_number,
                                  {{.name = L".", .parent_ref = root}}));
  for (ULONGLONG idx = static_cast<ULONGLONG>(Mft::Idx::User);
       idx < record_count; idx++) {
    PutScanRecord(image, idx, fragment_clusters, MakeBusySlotRecord(idx));
  }
  PutScanRecord(image, static_cast<ULONGLONG>(Mft::Idx::Mft), fragment_clusters,
                MakeScanMftRecord(record_count, fragment_clusters));
  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithListMftScan(ULONGLONG record_count,
                                      ULONGLONG fragment_clusters) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  PutScanRecord(image, static_cast<ULONGLONG>(Mft::Idx::Root),
                fragment_clusters,
                MakeMftTreeRecord(FileRecordFlag::InUse | FileRecordFlag::Dir,
                                  root_sequence_number,
                                  {{.name = L".", .parent_ref = root}}));
  const FileRecordFlag file = FileRecordFlag::InUse;
  ULONGLONG next_list_lcn = list_scan_lists_lcn;
  for (ULONGLONG idx = static_cast<ULONGLONG>(Mft::Idx::User);
       idx < record_count; idx++) {
    const std::wstring name = L"f" + std::to_wstring(idx);
    // The list of a base record names the slot after it, or before it.
    const auto base = [&](FileRecordFlag flags, ULONGLONG listed,
                          WORD sequence) {
      return MakeListScanBase(flags, ListScanBytes({listed, sequence}),
                              std::nullopt);
    };
    switch (idx % list_scan_kind_count) {
      case 1:
        PutScanRecord(image, idx, fragment_clusters,
                      base(file, idx + 1, list_scan_extension_sequence));
        break;
      case 2:
        PutScanRecord(image, idx, fragment_clusters,
                      MakeListScanExtension(file, list_scan_extension_sequence,
                                            idx, idx - 1));
        break;
      case 3:
        PutScanRecord(image, idx, fragment_clusters,
                      MakeListScanExtension(file, list_scan_extension_sequence,
                                            idx, idx + 1));
        break;
      case 4:
        PutScanRecord(image, idx, fragment_clusters,
                      base(file, idx - 1, list_scan_extension_sequence));
        break;
      case 5:
        // Names a file of its own: not an extension of this one.
        PutScanRecord(image, idx, fragment_clusters, base(file, idx + 1, 1));
        break;
      case 7:
        // Names a zero-filled slot, whose record the scan must read.
        PutScanRecord(image, idx, fragment_clusters,
                      base(file, idx + 1, list_scan_extension_sequence));
        break;
      case 8:
        PutScanRecord(image, idx, fragment_clusters, FakeRecord{});
        break;
      case 9: {
        // The list sits in a cluster of its own: reading it reads the volume.
        const std::vector<BYTE> list =
            ListScanBytes({idx + 1, list_scan_extension_sequence});
        const FakeDataRun run{.lcn = next_list_lcn++, .clusters = 1};
        PutScanRecord(image, idx, fragment_clusters,
                      MakeListScanBase(file, list, run));
        LayRunBytes(image, {run}, list);
        break;
      }
      case 10:
        PutScanRecord(image, idx, fragment_clusters,
                      MakeListScanExtension(file, list_scan_extension_sequence,
                                            idx, idx - 1));
        break;
      case 11:
        PutScanRecord(image, idx, fragment_clusters,
                      base(FileRecordFlag{}, idx + 1,
                           list_scan_freed_extension_sequence));
        break;
      case 12:
        PutScanRecord(image, idx, fragment_clusters,
                      MakeListScanExtension(FileRecordFlag{},
                                            list_scan_freed_extension_sequence,
                                            idx, idx - 1));
        break;
      case 13:
        PutScanRecord(image, idx, fragment_clusters,
                      MakeMftTreeRecord(file | FileRecordFlag::Dir, 1,
                                        {{.name = name, .parent_ref = root}}));
        break;
      default:
        // 0 and 6: files in use. 6 is what the list of 5 names.
        PutScanRecord(image, idx, fragment_clusters,
                      MakeMftTreeRecord(
                          file,
                          idx % list_scan_kind_count == 6 ? WORD{2} : WORD{1},
                          {{.name = name, .parent_ref = root}},
                          static_cast<DWORD>(idx % 100)));
        break;
    }
  }
  PutScanRecord(image, static_cast<ULONGLONG>(Mft::Idx::Mft), fragment_clusters,
                MakeScanMftRecord(record_count, fragment_clusters));
  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithIndexedMftScan(ULONGLONG record_count,
                                         size_t entries_per_block,
                                         ULONGLONG fragment_clusters) {
  std::vector<BYTE> image =
      BuildFakeNtfsImageWithFragmentedMftScan(record_count, fragment_clusters);

  // The volume's index blocks are as big as the fixture's.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = indexed_scan_block_clusters;

  const std::vector<IndexedScanEntry> entries =
      IndexedScanEntries(record_count);

  // The tree has two levels under the root: block 0 holds the separators, and
  // each leaf block the entries between two of them.
  std::vector<std::vector<FakeIndexName>> leaves;
  std::vector<FakeIndexName> separators;
  for (size_t next = 0; next < entries.size();) {
    const size_t left = entries.size() - next;
    // A last leaf of one entry would leave the separator before it with
    // nothing to separate: the leaf before it takes that entry.
    const size_t take =
        left <= entries_per_block + 1 ? left : entries_per_block;
    std::vector<FakeIndexName>& leaf = leaves.emplace_back();
    for (size_t i = 0; i < take; i++) {
      leaf.push_back(entries.at(next + i).fake);
    }
    next += take;
    if (next < entries.size()) {
      separators.push_back(entries.at(next).fake);
      next++;
    }
  }

  // The VCN of leaf j: block 0 is the separator node, at VCN 0.
  const auto leaf_vcn = [](size_t leaf) {
    return static_cast<ULONGLONG>(leaf + 1) * indexed_scan_block_clusters;
  };

  const size_t block_count = leaves.size() + 1;
  const size_t blocks_offset =
      static_cast<size_t>(indexed_scan_blocks_lcn) * cluster_size;
  PadToFullCacheRead(image,
                     blocks_offset + block_count * indexed_scan_block_size);

  const auto write_block = [&](size_t block, ULONGLONG vcn,
                               std::span<const FakeIndexName> names,
                               std::optional<ULONGLONG> last_vcn) {
    const std::span<BYTE> bytes = std::span<BYTE>(image).subspan(
        blocks_offset + block * indexed_scan_block_size,
        indexed_scan_block_size);
    auto& header = WriteIndexBlockHeader(bytes, indexed_scan_block_size, vcn);
    const DWORD size = WriteIndexedScanNode(
        bytes.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock)), names,
        last_vcn);
    header.total_entry_size = size;
    header.alloc_entry_size = size;
    header.not_leaf = last_vcn ? 1 : 0;
    ApplyIndexBlockFixup(bytes);
  };

  // Entry j of the separator node points at leaf j, which holds the names
  // before it, and the terminating entry points at the last leaf.
  for (size_t j = 0; j < separators.size(); j++) {
    separators.at(j).sub_node_vcn = leaf_vcn(j);
  }
  write_block(0, 0, separators, leaf_vcn(leaves.size() - 1));
  for (size_t j = 0; j < leaves.size(); j++) {
    write_block(j + 1, leaf_vcn(j), leaves.at(j), std::nullopt);
  }

  FakeRecord root = MakeInUseDirectoryRecord(root_sequence_number);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      root, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  // The root has the one entry that points at the separator node.
  offset += WriteSubNodeIndexRootAttr(root, offset, indexed_scan_block_size,
                                      indexed_scan_block_clusters);
  offset += WriteSingleRunIndexAllocAttr(
      root, offset,
      {.start_vcn = 0,
       .clusters =
           static_cast<DWORD>(block_count * indexed_scan_block_clusters),
       .lcn = indexed_scan_blocks_lcn},
      block_count * indexed_scan_block_size);
  WriteEndOfAttributesMarker(root, offset);
  PutScanRecord(image, static_cast<ULONGLONG>(Mft::Idx::Root),
                fragment_clusters, root);
  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
