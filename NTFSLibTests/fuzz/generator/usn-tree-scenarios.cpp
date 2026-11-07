#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/usn/usn-journal-record.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/resource-locator.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>
#include <ntfs-browser/tool/fuzzgen/usn-tree-scenarios.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeUsnOptions;
using FakeImage::FakeUsnRecord;
using Test::Usn::MakeRecord;

// One saved input: its file name, and the journal the image holds.
struct Variant {
  std::string name;
  FakeUsnOptions options;
};

// USN_REASON bits a record carries.
constexpr DWORD reason_data_overwrite = 0x00000001;
constexpr DWORD reason_file_create = 0x00000100;
constexpr DWORD reason_file_delete = 0x00000200;
constexpr DWORD reason_rename_old = 0x00001000;
constexpr DWORD reason_rename_new = 0x00002000;
constexpr DWORD reason_close = 0x80000000;

// FILE_ATTRIBUTE_DIRECTORY.
constexpr DWORD attribute_directory = 0x10;

// A record number the fixture has no record for, and the first of the numbers
// the journal-only files use.
constexpr ULONGLONG ghost_record = 40;
constexpr ULONGLONG first_journal_only_record = 41;

// Sequence number of the live report file in the fixture.
constexpr WORD report_sequence = 3;

// The root directory, and a record the fixture does not have. A journal-only
// file is filed under them.
constexpr ULONGLONG root_record = 5;
constexpr ULONGLONG missing_parent_record = 99;

// Times the records carry, increasing.
constexpr ULONGLONG time_a = 0x01D87F3C00000100ULL;
constexpr ULONGLONG time_b = 0x01D87F3C00000200ULL;
constexpr ULONGLONG time_c = 0x01D87F3C00000300ULL;

// A live file touched three times, and a freed one that was deleted.
std::vector<FakeUsnRecord> LiveAndFreedHistory() {
  return {MakeRecord({.name = L"report.txt",
                      .record = FakeImage::mft_tree_report_idx,
                      .sequence = report_sequence,
                      .reason = reason_file_create,
                      .time = time_a}),
          MakeRecord({.name = L"report.txt",
                      .record = FakeImage::mft_tree_report_idx,
                      .sequence = report_sequence,
                      .reason = reason_data_overwrite,
                      .time = time_b}),
          MakeRecord({.name = L"report.txt",
                      .record = FakeImage::mft_tree_report_idx,
                      .sequence = report_sequence,
                      .reason = reason_data_overwrite | reason_close,
                      .time = time_b}),
          MakeRecord({.name = L"old.tmp",
                      .record = FakeImage::mft_tree_deleted_file_idx,
                      .sequence = 1,
                      .reason = reason_file_create,
                      .time = time_a}),
          MakeRecord({.name = L"old.tmp",
                      .record = FakeImage::mft_tree_deleted_file_idx,
                      .sequence = 1,
                      .reason = reason_file_delete | reason_close,
                      .time = time_c})};
}

// A rename chain of a file no record holds.
std::vector<FakeUsnRecord> RenameChain() {
  return {MakeRecord({.name = L"a.txt",
                      .record = ghost_record,
                      .sequence = 2,
                      .reason = reason_file_create,
                      .time = time_a}),
          MakeRecord({.name = L"a.txt",
                      .record = ghost_record,
                      .sequence = 2,
                      .reason = reason_rename_old,
                      .time = time_b}),
          MakeRecord({.name = L"b.txt",
                      .record = ghost_record,
                      .sequence = 2,
                      .reason = reason_rename_new,
                      .time = time_b}),
          MakeRecord({.name = L"c.txt",
                      .record = ghost_record,
                      .sequence = 2,
                      .reason = reason_rename_new | reason_close,
                      .time = time_c})};
}

// The sequence rules, and a reference with a high half.
std::vector<FakeUsnRecord> SequenceRules() {
  FakeUsnRecord wide = MakeRecord({.name = L"wide",
                                   .record = FakeImage::mft_tree_report_idx,
                                   .sequence = report_sequence,
                                   .reason = reason_file_create,
                                   .time = time_a});
  wide.major_version = 3;
  wide.file_reference_high = 1;
  return {MakeRecord({.name = L"zero",
                      .record = FakeImage::mft_tree_hard_link_idx,
                      .sequence = 0,
                      .reason = reason_file_create,
                      .time = time_a}),
          MakeRecord({.name = L"older",
                      .record = FakeImage::mft_tree_report_idx,
                      .sequence = report_sequence - 1,
                      .reason = reason_file_create,
                      .time = time_b}),
          MakeRecord({.name = L"newer",
                      .record = FakeImage::mft_tree_report_idx,
                      .sequence = report_sequence + 1,
                      .reason = reason_file_create,
                      .time = time_c}),
          std::move(wide)};
}

// Records that name files the tree has: a live one touched three times, a
// freed one that was deleted, a rename chain of a file no record holds, the
// sequence rules, and a reference with a high half.
FakeUsnOptions Annotated() {
  FakeUsnOptions options{.records = LiveAndFreedHistory()};
  std::ranges::move(RenameChain(), std::back_inserter(options.records));
  std::ranges::move(SequenceRules(), std::back_inserter(options.records));
  return options;
}

// Files only the journal names, each under another kind of parent: a live
// directory, the root, a record that does not exist, a freed directory and a
// plain file. One of them is a directory itself.
FakeUsnOptions JournalOnlyPaths() {
  const ULONGLONG first = first_journal_only_record;
  FakeUsnRecord folder = MakeRecord({.name = L"folder",
                                     .record = first + 4,
                                     .sequence = 1,
                                     .reason = reason_file_create,
                                     .time = time_a});
  folder.file_attributes = attribute_directory;
  return {.records = {MakeRecord({.name = L"in-docs.bin",
                                  .record = first,
                                  .sequence = 1,
                                  .reason = reason_file_create,
                                  .time = time_a}),
                      MakeRecord({.name = L"in-root.bin",
                                  .record = first + 1,
                                  .sequence = 1,
                                  .reason = reason_file_create,
                                  .time = time_a,
                                  .parent = root_record,
                                  .parent_sequence = 5}),
                      MakeRecord({.name = L"in-lost.bin",
                                  .record = first + 2,
                                  .sequence = 1,
                                  .reason = reason_file_create,
                                  .time = time_a,
                                  .parent = missing_parent_record,
                                  .parent_sequence = 1}),
                      MakeRecord({.name = L"in-old-dir.bin",
                                  .record = first + 3,
                                  .sequence = 1,
                                  .reason = reason_file_create,
                                  .time = time_a,
                                  .parent = FakeImage::mft_tree_deleted_dir_idx,
                                  .parent_sequence = 3}),
                      MakeRecord({.name = L"in-file.bin",
                                  .record = first + 5,
                                  .sequence = 1,
                                  .reason = reason_file_create,
                                  .time = time_a,
                                  .parent = FakeImage::mft_tree_report_idx,
                                  .parent_sequence = report_sequence}),
                      std::move(folder)}};
}

// A short journal, for the variants that break the way to it.
FakeUsnOptions ShortJournal() {
  return {.records = {MakeRecord({.name = L"report.txt",
                                  .record = FakeImage::mft_tree_report_idx,
                                  .sequence = report_sequence,
                                  .reason = reason_file_create,
                                  .time = time_a}),
                      MakeRecord({.name = L"ghost.bin",
                                  .record = ghost_record,
                                  .sequence = 2,
                                  .reason = reason_file_delete | reason_close,
                                  .time = time_c})}};
}

// The inputs, one per way a journal meets the tree.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back({"usn_tree_annotated", Annotated()});
  variants.push_back({"usn_tree_journal_only_paths", JournalOnlyPaths()});

  FakeUsnOptions no_entry = ShortJournal();
  no_entry.omit_extend_entry = true;
  variants.push_back({"usn_loader_no_extend_entry", std::move(no_entry)});

  FakeUsnOptions no_record = ShortJournal();
  no_record.omit_journal_record = true;
  variants.push_back({"usn_loader_no_journal_record", std::move(no_record)});

  FakeUsnOptions no_stream = ShortJournal();
  no_stream.omit_j_stream = true;
  variants.push_back({"usn_loader_no_j_stream", std::move(no_stream)});

  // The cut shows only when a second run follows the first.
  FakeUsnOptions cut = ShortJournal();
  cut.more_stretches.push_back(
      {.hole_bytes = FakeImage::fake_cluster_size,
       .records = {MakeRecord({.name = L"later.txt",
                               .record = ghost_record + 1,
                               .sequence = 1,
                               .reason = reason_file_create,
                               .time = time_b})}});
  cut.cut_run_list = true;
  variants.push_back({"usn_loader_cut_run_list", std::move(cut)});
  return variants;
}

// Where the name of a V2 record starts: right after its fixed part.
constexpr std::size_t v2_name_offset = 60;

// The stream the journal scanner meets as a file of its own: a first sector
// that the boot sector patches wreck, then records at their own offsets, two of
// them with a name that is not UTF-16, and stray bytes after the last.
std::vector<BYTE> ScannerStream() {
  constexpr std::size_t damaged_head = 512;
  constexpr WORD lone_high_surrogate = 0xD800;
  constexpr WORD lone_low_surrogate = 0xDC00;
  constexpr BYTE stray = 0x5A;
  constexpr std::size_t stray_bytes = 3;

  std::vector<BYTE> stream(damaged_head, 0);
  const auto append = [&stream](const FakeUsnRecord& record, WORD unit) {
    std::vector<BYTE> bytes = FakeImage::EncodeUsnRecord(record, stream.size());
    if (unit != 0) {
      std::memcpy(&bytes.at(v2_name_offset), &unit, sizeof(unit));
    }
    stream.insert(stream.end(), bytes.begin(), bytes.end());
  };
  append(MakeRecord({.name = L"high0",
                     .record = 100,
                     .sequence = 1,
                     .reason = reason_file_create,
                     .time = time_a}),
         lone_high_surrogate);
  append(MakeRecord({.name = L"low00",
                     .record = 101,
                     .sequence = 1,
                     .reason = reason_file_create,
                     .time = time_a}),
         lone_low_surrogate);
  append(MakeRecord({.name = L"fine0",
                     .record = 102,
                     .sequence = 1,
                     .reason = reason_file_create,
                     .time = time_a}),
         0);
  stream.insert(stream.end(), stray_bytes, stray);
  return stream;
}

}  // namespace

void UsnTreeScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan read the
  // journal and list the files only it names.
  const auto options = RecordedStream::Recovering();
  for (const Variant& variant : Variants()) {
    registry.AddRecorded(
        variant.name,
        [journal = variant.options] {
          std::vector<BYTE> image =
              FakeImage::BuildFakeNtfsImageWithFreedFileTree();
          FakeImage::PutUsnJournal(image, journal);
          return image;
        },
        options);
  }

  // The fuzzer feeds every input to the journal scanner as well. This file is
  // the stream it finds, not a recording of an image.
  registry.Add({.name = "usn_scanner_damaged_names",
                .make = [](const ResourceLocator& /*resources*/) {
                  return ScannerStream();
                }});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
