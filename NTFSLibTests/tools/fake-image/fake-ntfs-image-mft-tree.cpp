#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "fake-ntfs-image-internal.h"

namespace NtfsBrowserTests {

std::vector<BYTE>
    BuildFakeNtfsImageWithOrphanedEntryNamedRecord(FakeNamedRecordState state) {
  std::vector<BYTE> image =
      BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch();

  // The "Orphan" entry already names
  // orphaned_block_sequence_mismatch_target_idx. Rewrite that record under the
  // entry's own sequence number, live or freed.
  const NtfsBrowser::Data::FileRecordFlag flags =
      state == FakeNamedRecordState::InUseSameSequence
          ? NtfsBrowser::Data::FileRecordFlag::InUse
          : NtfsBrowser::Data::FileRecordFlag{};
  const FakeRecord record =
      MakeMftTreeRecord(flags, orphaned_block_leaf_sequence_number, {});
  PutMftRecord(image, static_cast<DWORD>(mft_lcn) * cluster_size,
               orphaned_block_sequence_mismatch_target_idx, record);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftTree() {
  using NtfsBrowser::Data::FilenameNamespace;
  using NtfsBrowser::Data::StdInfoPermission;
  using RecordFlag = NtfsBrowser::Data::FileRecordFlag;

  // Sequence numbers the records below carry, where another record's
  // parent reference names them.
  constexpr WORD docs_sequence = 1;
  constexpr WORD old_dir_sequence = 3;
  constexpr WORD report_sequence = 3;
  constexpr WORD new_dir_sequence = 7;

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  const ULONGLONG docs = MakeFileReference(mft_tree_docs_idx, docs_sequence);

  // $MFT: a data run over every slot, written in place, so the records past
  // the first 16 read through it; plus the name real volumes give it.
  FakeRecord mft = MakeMftRecordWithRealDataRun(
      static_cast<DWORD>(mft_lcn), static_cast<DWORD>(mft_tree_record_count));
  EditFileRecordHeader(mft,
                       [](FileRecordHeader& header) { header.seq_no = 1; });
  DWORD offset = attr_offset_value +
                 reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
                     &mft.at(attr_offset_value))
                     ->header.total_size;
  offset += WriteFileNameAttr(mft, offset,
                              {.name = L"$MFT", .parent_ref = root}, false);
  WriteEndOfAttributesMarker(mft, offset);
  put_record(static_cast<ULONGLONG>(Mft::Idx::Mft), mft);

  const RecordFlag file = RecordFlag::InUse;
  const RecordFlag dir = RecordFlag::InUse | RecordFlag::Dir;
  const RecordFlag deleted_file{};
  const RecordFlag deleted_dir = RecordFlag::Dir;

  put_record(static_cast<ULONGLONG>(Mft::Idx::Root),
             MakeMftTreeRecord(dir, root_sequence_number,
                               {{.name = L".", .parent_ref = root}}));
  put_record(mft_tree_docs_idx,
             MakeMftTreeRecord(dir, docs_sequence,
                               {{.name = L"Docs", .parent_ref = root}}));
  put_record(mft_tree_report_idx,
             MakeMftTreeRecord(
                 file, report_sequence,
                 {{.name = L"report.txt",
                   .parent_ref = docs,
                   .real_size = mft_tree_report_stale_size,
                   .extra_flags = NtfsBrowser::Data::FilenameFlag::ReadOnly |
                                  NtfsBrowser::Data::FilenameFlag::Archive},
                  {.name = L"REPORT~1.TXT",
                   .parent_ref = docs,
                   .name_space = FilenameNamespace::Dos}},
                 mft_tree_report_data_size,
                 StdInfoPermission::ReadOnly | StdInfoPermission::Archive));
  put_record(mft_tree_hard_link_idx,
             MakeMftTreeRecord(file, 1,
                               {{.name = L"link-a", .parent_ref = root},
                                {.name = L"link-b", .parent_ref = docs}}));
  // NTFS bumps a record's sequence number when it frees the record.
  put_record(mft_tree_deleted_file_idx,
             MakeMftTreeRecord(deleted_file, 2,
                               {{.name = L"old.tmp", .parent_ref = docs}}));
  put_record(mft_tree_deleted_dir_idx,
             MakeMftTreeRecord(deleted_dir, old_dir_sequence + 1,
                               {{.name = L"OldDir", .parent_ref = docs}}));
  put_record(
      mft_tree_deleted_child_idx,
      MakeMftTreeRecord(deleted_file, 2,
                        {{.name = L"draft.doc",
                          .parent_ref = MakeFileReference(
                              mft_tree_deleted_dir_idx, old_dir_sequence)}}));
  put_record(mft_tree_stale_child_idx,
             MakeMftTreeRecord(
                 deleted_file, 2,
                 {{.name = L"stale.txt",
                   .parent_ref = MakeFileReference(mft_tree_reused_dir_idx,
                                                   new_dir_sequence - 1)}}));
  put_record(mft_tree_reused_dir_idx,
             MakeMftTreeRecord(dir, new_dir_sequence,
                               {{.name = L"NewDir", .parent_ref = root}}));
  put_record(mft_tree_extension_idx,
             MakeMftTreeRecord(
                 file, 1, {{.name = L"ext", .parent_ref = docs}}, {},
                 StdInfoPermission::Normal,
                 MakeFileReference(mft_tree_report_idx, report_sequence)));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftExtensionRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  // $MFT's sequence number, which BuildFakeNtfsImageWithMftTree() sets to 1.
  constexpr WORD mft_sequence = 1;
  const FakeRecord record = MakeMftTreeRecord(
      NtfsBrowser::Data::FileRecordFlag::InUse, mft_sequence, {}, {},
      NtfsBrowser::Data::StdInfoPermission::Normal,
      MakeFileReference(static_cast<ULONGLONG>(Mft::Idx::Mft), mft_sequence));

  const size_t offset = static_cast<size_t>(mft_lcn) * cluster_size +
                        static_cast<size_t>(fake_file_record_size) *
                            static_cast<size_t>(mft_tree_zeroed_idx);
  std::memcpy(&image.at(offset), record.data(), record.size());
  return image;
}

namespace {

// Directory record: $STANDARD_INFORMATION, a $FILE_NAME when name is set, and
// a resident $INDEX_ROOT listing names.
FakeRecord MakeIndexedDirRecord(WORD sequence,
                                const std::optional<FakeFileName>& name,
                                std::span<const FakeIndexName> names) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);
  EditFileRecordHeader(record, [sequence](FileRecordHeader& header) {
    header.seq_no = sequence;
  });

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NtfsBrowser::Data::StdInfoPermission::Normal);
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
      record, offset, NtfsBrowser::Data::StdInfoPermission::Normal);
  offset +=
      WriteFileNameAttr(record, offset,
                        {.name = L"stale.txt",
                         .parent_ref = MakeFileReference(mft_tree_docs_idx, 1)},
                        false);
  offset += WriteNonResidentAttr(record, offset, Attr::Type::Data, 0, real_size,
                                 runs);
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithFreedFileTree() {
  using NtfsBrowser::Data::FileRecordFlag;

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
      old_tmp, offset, NtfsBrowser::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(
      old_tmp, offset,
      {.name = L"old.tmp",
       .parent_ref = MakeFileReference(mft_tree_docs_idx, docs_sequence)},
      false);
  const DWORD data_offset = offset;
  offset += WriteResidentDataAttr(
      old_tmp, offset, static_cast<DWORD>(freed_file_content.size()));
  std::memcpy(
      &old_tmp.at(data_offset + sizeof(NtfsBrowser::Data::HeaderResident)),
      freed_file_content.data(), freed_file_content.size());
  WriteEndOfAttributesMarker(old_tmp, offset);
  put_record(mft_tree_deleted_file_idx, old_tmp);

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

}  // namespace NtfsBrowserTests
