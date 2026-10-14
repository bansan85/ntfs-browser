#include <cstring>
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

}  // namespace NtfsBrowserTests
