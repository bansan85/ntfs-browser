#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/standard-information.h"
#include "fake-ntfs-image-internal.h"
#include "file-record-header-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

std::vector<BYTE>
    BuildFakeNtfsImageWithOrphanedEntryNamedRecord(FakeNamedRecordState state) {
  std::vector<BYTE> image =
      BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch();

  // The "Orphan" entry already names
  // orphaned_block_sequence_mismatch_target_idx. Rewrite that record under the
  // entry's own sequence number, live or freed.
  const NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag flags =
      state == FakeNamedRecordState::InUseSameSequence
          ? NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse
          : NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag{};
  const FakeRecord record =
      MakeMftTreeRecord(flags, orphaned_block_leaf_sequence_number, {});
  PutMftRecord(image, static_cast<DWORD>(mft_lcn) * cluster_size,
               orphaned_block_sequence_mismatch_target_idx, record);

  return image;
}

namespace {

// The $MFT record of BuildFakeNtfsImageWithMftTree(): a data run over every
// record slot, and the name real volumes give it.
FakeRecord MakeMftTreeMftRecord() {
  FakeRecord mft = MakeMftRecordWithRealDataRun(
      static_cast<DWORD>(mft_lcn), static_cast<DWORD>(mft_tree_record_count));
  EditFileRecordHeader(mft,
                       [](FileRecordHeader& header) { header.seq_no = 1; });
  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  DWORD offset =
      attr_offset_value +
      reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &mft.at(attr_offset_value))
          ->header.total_size;
  offset += WriteFileNameAttr(mft, offset,
                              {.name = L"$MFT", .parent_ref = root}, false);
  WriteEndOfAttributesMarker(mft, offset);
  return mft;
}

// Sequence numbers the tree's records carry, where another record's parent
// reference names them.
constexpr WORD tree_docs_sequence = 1;
constexpr WORD tree_old_dir_sequence = 3;
constexpr WORD tree_report_sequence = 3;
constexpr WORD tree_new_dir_sequence = 7;

// Writes the root, the live directories and files, and the hard-linked file
// of BuildFakeNtfsImageWithMftTree() into image.
void PutMftTreeLiveRecords(std::vector<BYTE>& image) {
  using NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace;
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;
  using RecordFlag = NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    PutMftRecord(image, mft_addr, idx, record);
  };

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const ULONGLONG docs =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);

  const RecordFlag file = RecordFlag::InUse;
  const RecordFlag dir = RecordFlag::InUse | RecordFlag::Dir;

  put_record(static_cast<ULONGLONG>(Mft::Idx::Root),
             MakeMftTreeRecord(dir, root_sequence_number,
                               {{.name = L".", .parent_ref = root}}));
  put_record(mft_tree_docs_idx,
             MakeMftTreeRecord(dir, tree_docs_sequence,
                               {{.name = L"Docs", .parent_ref = root}}));
  put_record(mft_tree_report_idx,
             MakeMftTreeRecord(
                 file, tree_report_sequence,
                 {{.name = L"report.txt",
                   .parent_ref = docs,
                   .real_size = mft_tree_report_stale_size,
                   .extra_flags =
                       NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::ReadOnly |
                       NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::Archive},
                  {.name = L"REPORT~1.TXT",
                   .parent_ref = docs,
                   .name_space = FilenameNamespace::Dos}},
                 mft_tree_report_data_size,
                 StdInfoPermission::ReadOnly | StdInfoPermission::Archive));
  put_record(mft_tree_hard_link_idx,
             MakeMftTreeRecord(file, 1,
                               {{.name = L"link-a", .parent_ref = root},
                                {.name = L"link-b", .parent_ref = docs}}));
}

// Writes the freed, stale and reused directory records, and the extension
// record, of BuildFakeNtfsImageWithMftTree() into image.
void PutMftTreeFreedRecords(std::vector<BYTE>& image) {
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;
  using RecordFlag = NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    PutMftRecord(image, mft_addr, idx, record);
  };

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const ULONGLONG docs =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);

  const RecordFlag file = RecordFlag::InUse;
  const RecordFlag dir = RecordFlag::InUse | RecordFlag::Dir;
  const RecordFlag deleted_file{};
  const RecordFlag deleted_dir = RecordFlag::Dir;

  // NTFS bumps a record's sequence number when it frees the record.
  put_record(mft_tree_deleted_file_idx,
             MakeMftTreeRecord(deleted_file, 2,
                               {{.name = L"old.tmp", .parent_ref = docs}}));
  put_record(mft_tree_deleted_dir_idx,
             MakeMftTreeRecord(deleted_dir, tree_old_dir_sequence + 1,
                               {{.name = L"OldDir", .parent_ref = docs}}));
  put_record(mft_tree_deleted_child_idx,
             MakeMftTreeRecord(
                 deleted_file, 2,
                 {{.name = L"draft.doc",
                   .parent_ref = MakeFileReference(mft_tree_deleted_dir_idx,
                                                   tree_old_dir_sequence)}}));
  put_record(mft_tree_stale_child_idx,
             MakeMftTreeRecord(
                 deleted_file, 2,
                 {{.name = L"stale.txt",
                   .parent_ref = MakeFileReference(
                       mft_tree_reused_dir_idx, tree_new_dir_sequence - 1)}}));
  put_record(mft_tree_reused_dir_idx,
             MakeMftTreeRecord(dir, tree_new_dir_sequence,
                               {{.name = L"NewDir", .parent_ref = root}}));
  put_record(mft_tree_extension_idx,
             MakeMftTreeRecord(
                 file, 1, {{.name = L"ext", .parent_ref = docs}}, {},
                 StdInfoPermission::Normal,
                 MakeFileReference(mft_tree_report_idx, tree_report_sequence)));
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithMftTree() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  // $MFT goes in first, then the tree under it.
  PutMftRecord(image, static_cast<DWORD>(mft_lcn) * cluster_size,
               static_cast<ULONGLONG>(Mft::Idx::Mft), MakeMftTreeMftRecord());
  PutMftTreeLiveRecords(image);
  PutMftTreeFreedRecords(image);
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftExtensionRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  // $MFT's sequence number, which BuildFakeNtfsImageWithMftTree() sets to 1.
  constexpr WORD mft_sequence = 1;
  const FakeRecord record = MakeMftTreeRecord(
      NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse, mft_sequence, {}, {},
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal,
      MakeFileReference(static_cast<ULONGLONG>(Mft::Idx::Mft), mft_sequence));

  const size_t offset = static_cast<size_t>(mft_lcn) * cluster_size +
                        static_cast<size_t>(fake_file_record_size) *
                            static_cast<size_t>(mft_tree_zeroed_idx);
  std::memcpy(&image.at(offset), record.data(), record.size());
  return image;
}

void SetStdInfoPermission(std::vector<BYTE>& image, ULONGLONG idx,
                          DWORD permission) {
  const size_t record_offset =
      static_cast<size_t>(mft_lcn) * cluster_size +
      static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);

  NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader header{};
  std::memcpy(&header, &image.at(record_offset),
              NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                  min_file_record_header_size);
  const size_t attr_offset = record_offset + header.offset_of_attr;
  const auto& attr =
      *reinterpret_cast<const NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
          &image.at(attr_offset));

  const size_t permission_offset =
      attr_offset + attr.attr_offset +
      offsetof(NTFS_BROWSER_NAMESPACE::Data::StandardInformation, permission);
  std::memcpy(&image.at(permission_offset), &permission, sizeof(permission));
}

namespace {

// Directory record: $STANDARD_INFORMATION, a $FILE_NAME when name is set, and
// a resident $INDEX_ROOT listing names.
FakeRecord MakeIndexedDirRecord(WORD sequence,
                                const std::optional<FakeFileName>& name,
                                std::span<const FakeIndexName> names) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                             NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir);
  EditFileRecordHeader(record, [sequence](FileRecordHeader& header) {
    header.seq_no = sequence;
  });

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  if (name) {
    offset += WriteFileNameAttr(record, offset, *name, true);
  }
  offset += WriteIndexRootAttr(record, offset, names);
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Freed file record of sequence 2, its unnamed $DATA non-resident over runs.
FakeRecord
    MakeFreedNonResidentFileRecord(ULONGLONG real_size,
                                   const std::vector<FakeDataRun>& runs) {
  FakeRecord record = MakeRecordHeader(attr_offset_value, {});
  EditFileRecordHeader(record,
                       [](FileRecordHeader& header) { header.seq_no = 2; });

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset +=
      WriteFileNameAttr(record, offset,
                        {.name = L"stale.txt",
                         .parent_ref = MakeFileReference(mft_tree_docs_idx, 1)},
                        false);
  offset += WriteNonResidentAttr(
      record, offset, Attr::Type::Data,
      {.comp_unit_size = 0, .real_size = real_size, .runs = runs});
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithFreedFileTree() {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  // Sequence numbers the records below carry, where another record names them.
  constexpr WORD docs_sequence = 1;

  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);

  const std::array<FakeIndexName, 1> root_names{
      {{L"Docs", mft_tree_docs_idx, true}}};
  put_record(
      static_cast<ULONGLONG>(Mft::Idx::Root),
      MakeIndexedDirRecord(root_sequence_number,
                           FakeFileName{.name = L".", .parent_ref = root_ref},
                           root_names));

  const std::array<FakeIndexName, 2> docs_names{
      {{L"old.tmp", mft_tree_deleted_file_idx, false},
       {L"OldDir", mft_tree_deleted_dir_idx, true}}};
  put_record(mft_tree_docs_idx,
             MakeIndexedDirRecord(
                 docs_sequence,
                 FakeFileName{.name = L"Docs", .parent_ref = root_ref},
                 docs_names));

  // The freed "old.tmp": resident $DATA, written by hand so its content lands
  // in the record.
  FakeRecord old_tmp = MakeRecordHeader(attr_offset_value, FileRecordFlag{});
  EditFileRecordHeader(old_tmp,
                       [](FileRecordHeader& header) { header.seq_no = 2; });
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      old_tmp, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(
      old_tmp, offset,
      {.name = L"old.tmp",
       .parent_ref = MakeFileReference(mft_tree_docs_idx, docs_sequence)},
      false);
  const DWORD data_offset = offset;
  offset += WriteResidentDataAttr(
      old_tmp, offset, static_cast<DWORD>(freed_file_content.size()));
  std::memcpy(&old_tmp.at(data_offset +
                          sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident)),
              freed_file_content.data(), freed_file_content.size());
  WriteEndOfAttributesMarker(old_tmp, offset);
  put_record(mft_tree_deleted_file_idx, old_tmp);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithNamedRoot(std::wstring_view root_name) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const std::array<FakeIndexName, 2> root_names{
      {{L"Docs", mft_tree_docs_idx, true},
       {L"draft.doc", mft_tree_deleted_child_idx, false}}};
  put_record(static_cast<ULONGLONG>(Mft::Idx::Root),
             MakeIndexedDirRecord(
                 root_sequence_number,
                 FakeFileName{.name = root_name, .parent_ref = root_ref},
                 root_names));

  // The freed draft.doc keeps its sequence number, and now hangs off the root.
  put_record(
      mft_tree_deleted_child_idx,
      MakeMftTreeRecord(FileRecordFlag{}, 2,
                        {{.name = L"draft.doc", .parent_ref = root_ref}}));
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithIndexCycle() {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  // Sequence numbers the records below carry, where another record names them.
  constexpr WORD docs_sequence = 1;
  constexpr WORD new_dir_sequence = 7;
  constexpr WORD self_sequence = 1;

  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const ULONGLONG docs_ref =
      MakeFileReference(mft_tree_docs_idx, docs_sequence);
  const ULONGLONG self_ref =
      MakeFileReference(mft_tree_zeroed_idx, self_sequence);

  // Docs is left out of the root, so Docs has no parent but NewDir.
  const std::array<FakeIndexName, 2> root_names{
      {{L".", static_cast<ULONGLONG>(Mft::Idx::Root), true},
       {L"Self", mft_tree_zeroed_idx, true}}};
  put_record(
      static_cast<ULONGLONG>(Mft::Idx::Root),
      MakeIndexedDirRecord(root_sequence_number,
                           FakeFileName{.name = L".", .parent_ref = root_ref},
                           root_names));

  const std::array<FakeIndexName, 2> docs_names{
      {{L"NewDir", mft_tree_reused_dir_idx, true},
       {L"old.tmp", mft_tree_deleted_file_idx, false}}};
  put_record(mft_tree_docs_idx,
             MakeIndexedDirRecord(
                 docs_sequence,
                 FakeFileName{.name = L"Docs", .parent_ref = root_ref},
                 docs_names));

  const std::array<FakeIndexName, 1> new_dir_names{
      {{L"Docs", mft_tree_docs_idx, true}}};
  put_record(mft_tree_reused_dir_idx,
             MakeIndexedDirRecord(
                 new_dir_sequence,
                 FakeFileName{.name = L"NewDir", .parent_ref = docs_ref},
                 new_dir_names));

  // "." names Self itself, which the search must not treat as a parent.
  const std::array<FakeIndexName, 2> self_names{
      {{L".", mft_tree_zeroed_idx, true},
       {L"self.tmp", mft_tree_extension_idx, false}}};
  put_record(mft_tree_zeroed_idx,
             MakeIndexedDirRecord(
                 self_sequence,
                 FakeFileName{.name = L"Self", .parent_ref = root_ref},
                 self_names));

  put_record(
      mft_tree_extension_idx,
      MakeMftTreeRecord(FileRecordFlag{}, self_sequence,
                        {{.name = L"self.tmp", .parent_ref = self_ref}}));
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFreedNonResidentFile(
    ULONGLONG real_size, const std::vector<FakeDataRun>& runs) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const FakeRecord record = MakeFreedNonResidentFileRecord(real_size, runs);
  const size_t offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     gsl::narrow<size_t>(mft_tree_stale_child_idx);
  std::memcpy(&image.at(offset), record.data(), record.size());
  return image;
}

namespace {

// In-use file record of sequence sequence, whose unnamed $DATA is resident and
// holds content.
FakeRecord MakeResidentFileRecord(WORD sequence, const FakeFileName& name,
                                  std::string_view content) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  EditFileRecordHeader(record, [sequence](FileRecordHeader& header) {
    header.seq_no = sequence;
  });

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(record, offset, name, false);
  const DWORD data_offset = offset;
  offset +=
      WriteResidentDataAttr(record, offset, static_cast<DWORD>(content.size()));
  // An empty view may carry a null data(); memcpy forbids that even at size 0.
  if (!content.empty()) {
    std::memcpy(
        &record.at(data_offset +
                   sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident)),
        content.data(), content.size());
  }
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// In-use file record of sequence sequence, whose unnamed $DATA is non-resident
// over runs.
FakeRecord MakeNonResidentFileRecord(WORD sequence, const FakeFileName& name,
                                     ULONGLONG real_size,
                                     const std::vector<FakeDataRun>& runs) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  EditFileRecordHeader(record, [sequence](FileRecordHeader& header) {
    header.seq_no = sequence;
  });

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(record, offset, name, false);
  offset += WriteNonResidentAttr(
      record, offset, Attr::Type::Data,
      {.comp_unit_size = 0, .real_size = real_size, .runs = runs});
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// True when every real run of runs lies inside the clusters image holds.
bool RunsInsideImage(const std::vector<BYTE>& image,
                     const std::vector<FakeDataRun>& runs) {
  const ULONGLONG image_clusters = image.size() / cluster_size;
  return std::ranges::all_of(runs, [image_clusters](const FakeDataRun& run) {
    return !run.lcn || *run.lcn + run.clusters <= image_clusters;
  });
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithDumpTree(ULONGLONG big_real_size,
                                   const std::vector<FakeDataRun>& big_runs) {
  // Sequence numbers the records below carry, where another record names them.
  constexpr WORD docs_sequence = 1;
  constexpr WORD report_sequence = 3;
  constexpr WORD file_sequence = 1;

  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const ULONGLONG docs_ref =
      MakeFileReference(mft_tree_docs_idx, docs_sequence);

  // Index entries stay in collation order, as the lookups expect. The root
  // lists itself as "." too, as a real volume's does.
  const std::array<FakeIndexName, 4> root_names{
      {{L".", static_cast<ULONGLONG>(Mft::Idx::Root), true,
        root_sequence_number},
       {L"big.bin", dump_big_file_idx, false},
       {L"Docs", mft_tree_docs_idx, true},
       {L"empty.txt", dump_empty_file_idx, false}}};
  put_record(
      static_cast<ULONGLONG>(Mft::Idx::Root),
      MakeIndexedDirRecord(root_sequence_number,
                           FakeFileName{.name = L".", .parent_ref = root_ref},
                           root_names));

  const std::array<FakeIndexName, 4> docs_names{
      {{.name = L"Broken",
        .mft_ref = mft_tree_zeroed_idx,
        .directory = false,
        .real_size = dump_broken_real_size,
        .alloc_size = dump_broken_alloc_size,
        .alter_time = dump_broken_alter_time},
       {L"old.tmp", mft_tree_deleted_file_idx, false},
       {L"OldDir", mft_tree_deleted_dir_idx, true},
       {L"report.txt", mft_tree_report_idx, false, report_sequence}}};
  put_record(mft_tree_docs_idx,
             MakeIndexedDirRecord(
                 docs_sequence,
                 FakeFileName{.name = L"Docs", .parent_ref = root_ref},
                 docs_names));

  put_record(
      mft_tree_report_idx,
      MakeResidentFileRecord(report_sequence,
                             {.name = L"report.txt", .parent_ref = docs_ref},
                             dump_report_content));
  put_record(dump_big_file_idx,
             MakeNonResidentFileRecord(
                 file_sequence, {.name = L"big.bin", .parent_ref = root_ref},
                 big_real_size, big_runs));
  put_record(dump_empty_file_idx,
             MakeResidentFileRecord(
                 file_sequence, {.name = L"empty.txt", .parent_ref = root_ref},
                 std::string_view{}));

  // Counting bytes over the big file's clusters, so a dump shows the file
  // offset of every byte. Written only when every run is backed, so a run past
  // the base image stays unbacked.
  if (RunsInsideImage(image, big_runs)) {
    std::size_t backed_bytes = 0;
    for (const FakeDataRun& run : big_runs) {
      if (run.lcn) {
        backed_bytes += static_cast<std::size_t>(run.clusters) * cluster_size;
      }
    }
    std::vector<BYTE> pattern(backed_bytes);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
      pattern[i] = static_cast<BYTE>(i & 0xFFU);
    }
    LayRunBytes(image, big_runs, pattern);
  }

  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
