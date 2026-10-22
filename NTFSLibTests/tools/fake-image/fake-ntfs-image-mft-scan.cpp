#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
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

// Writes record, the content of $MFT slot idx, in the fragment that holds it.
void PutScanRecord(std::vector<BYTE>& image, ULONGLONG idx,
                   ULONGLONG fragment_clusters, const FakeRecord& record) {
  PutRecordAt(
      image,
      static_cast<size_t>(FragmentedMftScanSlotOffset(idx, fragment_clusters)),
      record);
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

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
