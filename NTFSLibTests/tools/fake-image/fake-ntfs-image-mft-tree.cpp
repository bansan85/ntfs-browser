#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/attribute-list.h"
#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/standard-information.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

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
  PutMftRecord(image, orphaned_block_sequence_mismatch_target_idx, record);

  return image;
}

namespace {

// The $MFT record of BuildFakeNtfsImageWithMftTree(): a data run over every
// record slot, and the name real volumes give it.
// extra_attr, when set, writes one more attribute after $FILE_NAME and returns
// its total_size.
FakeRecord MakeMftTreeMftRecord(
    const std::function<DWORD(FakeRecord&, DWORD)>& extra_attr = {}) {
  FakeRecord mft = MakeMftRecordWithRealDataRun(
      static_cast<DWORD>(mft_lcn), static_cast<DWORD>(mft_tree_record_count));
  EditFileRecordHeader(mft,
                       [](FileRecordHeader& header) { header.seq_no = 1; });
  const ULONGLONG root = root_file_reference;
  DWORD offset =
      attr_offset_value +
      reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &mft.at(attr_offset_value))
          ->header.total_size;
  offset += WriteFileNameAttr(mft, offset,
                              {.name = L"$MFT", .parent_ref = root}, false);
  if (extra_attr) {
    offset += extra_attr(mft, offset);
  }
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

  const ULONGLONG root = root_file_reference;
  const ULONGLONG docs =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);

  const RecordFlag file = RecordFlag::InUse;
  const RecordFlag dir = RecordFlag::InUse | RecordFlag::Dir;

  PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Root),
               MakeMftTreeRecord(dir, root_sequence_number,
                                 {{.name = L".", .parent_ref = root}}));
  PutMftRecord(image, mft_tree_docs_idx,
               MakeMftTreeRecord(dir, tree_docs_sequence,
                                 {{.name = L"Docs", .parent_ref = root}}));
  PutMftRecord(image, mft_tree_report_idx,
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
  PutMftRecord(image, mft_tree_hard_link_idx,
               MakeMftTreeRecord(file, 1,
                                 {{.name = L"link-a", .parent_ref = root},
                                  {.name = L"link-b", .parent_ref = docs}}));
}

// Writes the freed, stale and reused directory records, and the extension
// record, of BuildFakeNtfsImageWithMftTree() into image.
void PutMftTreeFreedRecords(std::vector<BYTE>& image) {
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;
  using RecordFlag = NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  const ULONGLONG root = root_file_reference;
  const ULONGLONG docs =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);

  const RecordFlag file = RecordFlag::InUse;
  const RecordFlag dir = RecordFlag::InUse | RecordFlag::Dir;
  const RecordFlag deleted_file{};
  const RecordFlag deleted_dir = RecordFlag::Dir;

  // NTFS bumps a record's sequence number when it frees the record.
  PutMftRecord(image, mft_tree_deleted_file_idx,
               MakeMftTreeRecord(deleted_file, 2,
                                 {{.name = L"old.tmp", .parent_ref = docs}}));
  PutMftRecord(image, mft_tree_deleted_dir_idx,
               MakeMftTreeRecord(deleted_dir, tree_old_dir_sequence + 1,
                                 {{.name = L"OldDir", .parent_ref = docs}}));
  PutMftRecord(image, mft_tree_deleted_child_idx,
               MakeMftTreeRecord(
                   deleted_file, 2,
                   {{.name = L"draft.doc",
                     .parent_ref = MakeFileReference(mft_tree_deleted_dir_idx,
                                                     tree_old_dir_sequence)}}));
  PutMftRecord(image, mft_tree_stale_child_idx,
               MakeMftTreeRecord(deleted_file, 2,
                                 {{.name = L"stale.txt",
                                   .parent_ref = MakeFileReference(
                                       mft_tree_reused_dir_idx,
                                       tree_new_dir_sequence - 1)}}));
  PutMftRecord(image, mft_tree_reused_dir_idx,
               MakeMftTreeRecord(dir, tree_new_dir_sequence,
                                 {{.name = L"NewDir", .parent_ref = root}}));
  PutMftRecord(image, mft_tree_extension_idx,
               MakeMftTreeRecord(file, 1,
                                 {{.name = L"ext", .parent_ref = docs}}, {},
                                 StdInfoPermission::Normal,
                                 MakeFileReference(mft_tree_report_idx,
                                                   tree_report_sequence)));
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithMftTree() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  // $MFT goes in first, then the tree under it.
  PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftTreeMftRecord());
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

  PutMftRecord(image, mft_tree_zeroed_idx, record);
  return image;
}

namespace {

// Sequence numbers of the records BuildFakeNtfsImageWithMftTreeAttributeList()
// writes, distinct so that a wrong file reference shows.
constexpr WORD tree_list_base_sequence = 4;
constexpr WORD tree_list_extension_sequence = 2;
// The sequence NTFS gives the extensions it frees with their file: one more.
constexpr WORD tree_freed_extension_sequence = 3;

// One entry of the $ATTRIBUTE_LIST of mft_tree_list_base_idx.
struct FakeTreeListEntry {
  Attr::Type type;
  ULONGLONG record;
};

// Bytes of one entry of that list.
constexpr auto tree_list_entry_size = static_cast<WORD>(AlignAttrSize(
    NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size));

// The bytes of an $ATTRIBUTE_LIST of entries, each naming its record under
// tree_list_extension_sequence.
std::vector<BYTE> TreeListBytes(std::span<const FakeTreeListEntry> entries) {
  std::vector<BYTE> bytes(tree_list_entry_size * entries.size(), 0);
  for (size_t i = 0; i < entries.size(); i++) {
    NTFS_BROWSER_NAMESPACE::Data::AttributeList entry{};
    entry.attr_type = gsl::at(entries, i).type;
    entry.record_size = tree_list_entry_size;
    entry.base_ref.segment_number = gsl::at(entries, i).record;
    entry.base_ref.sequence_number = tree_list_extension_sequence;
    std::memcpy(&bytes.at(i * tree_list_entry_size), &entry,
                NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size);
  }
  return bytes;
}

// Writes a resident $ATTRIBUTE_LIST holding list at record[offset], and
// returns its total_size.
DWORD WriteTreeAttributeList(FakeRecord& record, DWORD offset,
                             const std::vector<BYTE>& list) {
  auto& attr = InitResidentAttr(record, offset, Attr::Type::AttributeList);
  attr.attr_size = gsl::narrow<DWORD>(list.size());
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
  std::memcpy(&record.at(offset + attr.attr_offset), list.data(), list.size());
  return attr.header.total_size;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithMftTreeAttributeList(
    FakeListedExtension high, FakeListResidency list, FakeListedState listed) {
  using RecordFlag = NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;

  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();
  const bool freed = listed == FakeListedState::Freed;
  const ULONGLONG base_ref =
      MakeFileReference(mft_tree_list_base_idx, tree_list_base_sequence);
  ULONGLONG high_base_ref = base_ref;
  if (high == FakeListedExtension::Foreign) {
    high_base_ref = MakeFileReference(mft_tree_hard_link_idx, 1);
  } else if (high == FakeListedExtension::Base) {
    high_base_ref = 0;
  }
  const ULONGLONG docs =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);
  // A Foreign extension is another file's record: it keeps its own state.
  const bool high_freed = freed && high == FakeListedExtension::Genuine;
  const RecordFlag live_flags = RecordFlag::InUse;
  const RecordFlag freed_flags{};

  PutMftRecord(
      image, mft_tree_list_low_ext_idx,
      MakeMftTreeRecord(
          freed ? freed_flags : live_flags,
          freed ? tree_freed_extension_sequence : tree_list_extension_sequence,
          {{.name = L"listed.txt", .parent_ref = root_file_reference}}, {},
          StdInfoPermission::Normal, base_ref));

  FakeRecord base =
      MakeRecordHeader(attr_offset_value, freed ? freed_flags : live_flags,
                       tree_list_base_sequence);
  DWORD offset = attr_offset_value;
  offset +=
      WriteStandardInformationAttr(base, offset, StdInfoPermission::Normal);
  const std::array<FakeTreeListEntry, 3> entries{
      {{.type = Attr::Type::FileName, .record = mft_tree_list_low_ext_idx},
       {.type = Attr::Type::FileName, .record = mft_tree_list_high_ext_idx},
       {.type = Attr::Type::Data, .record = mft_tree_list_high_ext_idx}}};
  const std::vector<BYTE> list_bytes = TreeListBytes(entries);
  const std::vector<FakeDataRun> list_runs{{mft_tree_list_lcn, 1}};
  if (list == FakeListResidency::Resident) {
    offset += WriteTreeAttributeList(base, offset, list_bytes);
  } else {
    offset += WriteNonResidentAttr(base, offset, Attr::Type::AttributeList,
                                   {.comp_unit_size = 0,
                                    .real_size = list_bytes.size(),
                                    .runs = list_runs});
  }
  WriteEndOfAttributesMarker(base, offset);
  PutMftRecord(image, mft_tree_list_base_idx, base);
  if (list == FakeListResidency::NonResident) {
    LayRunBytes(image, list_runs, list_bytes);
  }

  PutMftRecord(
      image, mft_tree_list_high_ext_idx,
      MakeMftTreeRecord(high_freed ? freed_flags : live_flags,
                        high_freed ? tree_freed_extension_sequence
                                   : tree_list_extension_sequence,
                        {{.name = L"listed-link.txt", .parent_ref = docs}},
                        mft_tree_list_data_size, StdInfoPermission::Normal,
                        high_base_ref));
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftTreeStrayExtension() {
  using NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission;

  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTreeAttributeList();
  PutMftRecord(image, mft_tree_report_idx,
               MakeMftTreeRecord(
                   NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse, 1,
                   {{.name = L"stray.txt", .parent_ref = root_file_reference}},
                   {}, StdInfoPermission::Normal,
                   MakeFileReference(mft_tree_hard_link_idx, 1)));
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
  FakeRecord record = MakeInUseDirectoryRecord(sequence);

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

// The root directory record of the freed-file trees, named name and listing
// names.
FakeRecord MakeRootDirRecord(std::wstring_view name,
                             std::span<const FakeIndexName> names) {
  return MakeIndexedDirRecord(
      root_sequence_number,
      FakeFileName{.name = name, .parent_ref = root_file_reference}, names);
}

// The "Docs" directory record of the freed-file trees, listing names.
FakeRecord MakeDocsDirRecord(std::span<const FakeIndexName> names) {
  return MakeIndexedDirRecord(
      tree_docs_sequence,
      FakeFileName{.name = L"Docs", .parent_ref = root_file_reference}, names);
}

// Freed file record of sequence 2, its unnamed $DATA non-resident as stream
// describes.
FakeRecord MakeFreedNonResidentFileRecord(const FakeFreedStream& stream) {
  FakeRecord record = MakeRecordHeader(attr_offset_value, {}, 2);

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset +=
      WriteFileNameAttr(record, offset,
                        {.name = L"stale.txt",
                         .parent_ref = MakeFileReference(mft_tree_docs_idx, 1)},
                        false);
  offset += WriteNonResidentAttr(record, offset, Attr::Type::Data,
                                 {.comp_unit_size = 0,
                                  .real_size = stream.real_size,
                                  .runs = stream.runs,
                                  .overrides = {.last_vcn = stream.last_vcn,
                                                .raw_runs = stream.raw_runs}});
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithFreedFileTree() {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  const std::array<FakeIndexName, 1> root_names{
      {{L"Docs", mft_tree_docs_idx, true}}};
  PutRootRecord(image, MakeRootDirRecord(L".", root_names));

  const std::array<FakeIndexName, 2> docs_names{
      {{L"old.tmp", mft_tree_deleted_file_idx, false},
       {L"OldDir", mft_tree_deleted_dir_idx, true}}};
  PutMftRecord(image, mft_tree_docs_idx, MakeDocsDirRecord(docs_names));

  // The freed "old.tmp": resident $DATA, written by hand so its content lands
  // in the record.
  FakeRecord old_tmp = MakeRecordHeader(attr_offset_value, FileRecordFlag{}, 2);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      old_tmp, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(
      old_tmp, offset,
      {.name = L"old.tmp",
       .parent_ref = MakeFileReference(mft_tree_docs_idx, tree_docs_sequence)},
      false);
  const DWORD data_offset = offset;
  offset += WriteResidentDataAttr(
      old_tmp, offset, static_cast<DWORD>(freed_file_content.size()));
  std::memcpy(&old_tmp.at(data_offset +
                          sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident)),
              freed_file_content.data(), freed_file_content.size());
  WriteEndOfAttributesMarker(old_tmp, offset);
  PutMftRecord(image, mft_tree_deleted_file_idx, old_tmp);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithNamedRoot(std::wstring_view root_name) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const std::array<FakeIndexName, 2> root_names{
      {{L"Docs", mft_tree_docs_idx, true},
       {L"draft.doc", mft_tree_deleted_child_idx, false}}};
  PutRootRecord(image, MakeRootDirRecord(root_name, root_names));

  // The freed draft.doc keeps its sequence number, and now hangs off the root.
  PutMftRecord(image, mft_tree_deleted_child_idx,
               MakeMftTreeRecord(FileRecordFlag{}, 2,
                                 {{.name = L"draft.doc",
                                   .parent_ref = root_file_reference}}));
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithIndexCycle() {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;

  // Sequence numbers the records below carry, where another record names them.
  constexpr WORD new_dir_sequence = 7;
  constexpr WORD self_sequence = 1;

  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const ULONGLONG docs_ref =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);
  const ULONGLONG self_ref =
      MakeFileReference(mft_tree_zeroed_idx, self_sequence);

  // Docs is left out of the root, so Docs has no parent but NewDir.
  const std::array<FakeIndexName, 2> root_names{
      {{L".", static_cast<ULONGLONG>(Mft::Idx::Root), true},
       {L"Self", mft_tree_zeroed_idx, true}}};
  PutRootRecord(image, MakeRootDirRecord(L".", root_names));

  const std::array<FakeIndexName, 2> docs_names{
      {{L"NewDir", mft_tree_reused_dir_idx, true},
       {L"old.tmp", mft_tree_deleted_file_idx, false}}};
  PutMftRecord(image, mft_tree_docs_idx, MakeDocsDirRecord(docs_names));

  const std::array<FakeIndexName, 1> new_dir_names{
      {{L"Docs", mft_tree_docs_idx, true}}};
  PutMftRecord(image, mft_tree_reused_dir_idx,
               MakeIndexedDirRecord(
                   new_dir_sequence,
                   FakeFileName{.name = L"NewDir", .parent_ref = docs_ref},
                   new_dir_names));

  // "." names Self itself, which the search must not treat as a parent.
  const std::array<FakeIndexName, 2> self_names{
      {{L".", mft_tree_zeroed_idx, true},
       {L"self.tmp", mft_tree_extension_idx, false}}};
  PutMftRecord(
      image, mft_tree_zeroed_idx,
      MakeIndexedDirRecord(
          self_sequence,
          FakeFileName{.name = L"Self", .parent_ref = root_file_reference},
          self_names));

  PutMftRecord(
      image, mft_tree_extension_idx,
      MakeMftTreeRecord(FileRecordFlag{}, self_sequence,
                        {{.name = L"self.tmp", .parent_ref = self_ref}}));
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFreedNonResidentFile(
    ULONGLONG real_size, const std::vector<FakeDataRun>& runs) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();
  PutFreedNonResidentRecord(image, mft_tree_stale_child_idx,
                            {.real_size = real_size, .runs = runs});
  return image;
}

void PutFreedNonResidentRecord(std::vector<BYTE>& image, ULONGLONG idx,
                               const FakeFreedStream& stream) {
  PutMftRecord(image, idx, MakeFreedNonResidentFileRecord(stream));
}

namespace {

// In-use file record of sequence sequence, whose unnamed $DATA is resident and
// holds content.
FakeRecord MakeResidentFileRecord(WORD sequence, const FakeFileName& name,
                                  std::string_view content) {
  FakeRecord record = MakeInUseFileRecord(sequence);

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
  FakeRecord record = MakeInUseFileRecord(sequence);

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

std::vector<BYTE> BuildFakeNtfsImageWithReportName(std::wstring_view name,
                                                   ULONGLONG alter_time) {
  // Sequence number the report record carries, where the Docs index names it.
  constexpr WORD report_sequence = 3;

  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const ULONGLONG docs_ref =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);

  const std::array<FakeIndexName, 1> docs_names{
      {{.name = name,
        .mft_ref = mft_tree_report_idx,
        .directory = false,
        .sequence = report_sequence,
        .alter_time = alter_time}}};
  PutMftRecord(image, mft_tree_docs_idx, MakeDocsDirRecord(docs_names));
  PutMftRecord(image, mft_tree_report_idx,
               MakeResidentFileRecord(report_sequence,
                                      {.name = name, .parent_ref = docs_ref},
                                      dump_report_content));
  return image;
}

namespace {

// Lays counting bytes over the clusters of the real runs, so a dump shows the
// file offset of every byte. Written only when every run is inside the image,
// so a run past the base image stays unbacked.
void LayCountingBytes(std::vector<BYTE>& image,
                      const std::vector<FakeDataRun>& runs) {
  if (!RunsInsideImage(image, runs)) {
    return;
  }
  std::size_t backed_bytes = 0;
  for (const FakeDataRun& run : runs) {
    if (run.lcn) {
      backed_bytes += static_cast<std::size_t>(run.clusters) * cluster_size;
    }
  }
  std::vector<BYTE> pattern(backed_bytes);
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = static_cast<BYTE>(i & 0xFFU);
  }
  LayRunBytes(image, runs, pattern);
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithDumpTree(ULONGLONG big_real_size,
                                   const std::vector<FakeDataRun>& big_runs) {
  // Sequence numbers the records below carry, where another record names them.
  constexpr WORD report_sequence = 3;
  constexpr WORD file_sequence = 1;

  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();

  const ULONGLONG docs_ref =
      MakeFileReference(mft_tree_docs_idx, tree_docs_sequence);

  // Index entries stay in collation order, as the lookups expect. The root
  // lists itself as "." too, as a real volume's does.
  const std::array<FakeIndexName, 4> root_names{
      {{L".", static_cast<ULONGLONG>(Mft::Idx::Root), true,
        root_sequence_number},
       {L"big.bin", dump_big_file_idx, false},
       {L"Docs", mft_tree_docs_idx, true},
       {L"empty.txt", dump_empty_file_idx, false}}};
  PutRootRecord(image, MakeRootDirRecord(L".", root_names));

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
  PutMftRecord(image, mft_tree_docs_idx, MakeDocsDirRecord(docs_names));

  PutMftRecord(
      image, mft_tree_report_idx,
      MakeResidentFileRecord(report_sequence,
                             {.name = L"report.txt", .parent_ref = docs_ref},
                             dump_report_content));
  PutMftRecord(image, dump_big_file_idx,
               MakeNonResidentFileRecord(
                   file_sequence,
                   {.name = L"big.bin", .parent_ref = root_file_reference},
                   big_real_size, big_runs));
  PutMftRecord(image, dump_empty_file_idx,
               MakeResidentFileRecord(
                   file_sequence,
                   {.name = L"empty.txt", .parent_ref = root_file_reference},
                   std::string_view{}));

  LayCountingBytes(image, big_runs);
  return image;
}

namespace {

// Where the non-resident $BITMAP of the fixtures below lies: past the $MFT's
// clusters, inside the image's first 64 KiB.
constexpr DWORD mft_bitmap_lcn = 60;

// A cluster number no image holds, so reading it fails.
constexpr DWORD mft_bitmap_unreadable_lcn = 1U << 20U;

// Bytes of the bitmap that matches the fixture: mft_tree_bitmap_used_idx set.
std::vector<BYTE> AccurateMftBitmap() {
  std::vector<BYTE> bitmap(AlignAttrSize((mft_tree_record_count + 7) / 8),
                           BYTE{0});
  for (const ULONGLONG idx : mft_tree_bitmap_used_idx) {
    bitmap.at(idx / 8) |= static_cast<BYTE>(1U << (idx % 8));
  }
  return bitmap;
}

// Writes a resident $BITMAP holding bytes at record[offset] and returns its
// total_size.
DWORD WriteResidentBitmapAttr(FakeRecord& record, DWORD offset,
                              const std::vector<BYTE>& bytes) {
  auto& attr = InitResidentAttr(record, offset, Attr::Type::Bitmap);
  attr.attr_size = gsl::narrow<DWORD>(bytes.size());
  attr.header.total_size = AlignAttrSize(sizeof(attr) + bytes.size());
  std::memcpy(&record.at(offset + attr.attr_offset), bytes.data(),
              bytes.size());
  return attr.header.total_size;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithMftBitmap(FakeMftBitmap kind) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();
  std::vector<BYTE> bitmap = AccurateMftBitmap();
  std::function<DWORD(FakeRecord&, DWORD)> write_bitmap;

  if (kind == FakeMftBitmap::Lying) {
    // In use but marked free, and freed but marked used.
    bitmap.at(mft_tree_report_idx / 8) &=
        static_cast<BYTE>(~(1U << (mft_tree_report_idx % 8)));
    bitmap.at(mft_tree_deleted_file_idx / 8) |=
        static_cast<BYTE>(1U << (mft_tree_deleted_file_idx % 8));
  } else if (kind == FakeMftBitmap::TooShort) {
    // Covers records 0-15 only.
    bitmap.resize(2);
  }

  if (kind == FakeMftBitmap::NonResident || kind == FakeMftBitmap::Unreadable) {
    const DWORD lcn = kind == FakeMftBitmap::NonResident
                          ? mft_bitmap_lcn
                          : mft_bitmap_unreadable_lcn;
    write_bitmap = [lcn, &bitmap](FakeRecord& record, DWORD offset) {
      return WriteNonResidentAttr(
          record, offset, Attr::Type::Bitmap,
          {.real_size = bitmap.size(), .runs = {{lcn, 1}}});
    };
    if (kind == FakeMftBitmap::NonResident) {
      std::memcpy(&image.at(static_cast<size_t>(mft_bitmap_lcn) * cluster_size),
                  bitmap.data(), bitmap.size());
    }
  } else {
    write_bitmap = [&bitmap](FakeRecord& record, DWORD offset) {
      return WriteResidentBitmapAttr(record, offset, bitmap);
    };
  }

  PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftTreeMftRecord(write_bitmap));
  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
