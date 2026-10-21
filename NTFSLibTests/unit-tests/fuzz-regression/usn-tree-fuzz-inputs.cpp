#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "fuzz-input-writer.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeUsnOptions;
using Tests::FakeUsnRecord;

namespace {

// One saved fuzz input: its file name, and the journal the image holds.
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

// FILE_ATTRIBUTE_DIRECTORY and FILE_ATTRIBUTE_ARCHIVE.
constexpr DWORD attribute_directory = 0x10;
constexpr DWORD attribute_archive = 0x20;

// A record number the fixture has no record for, and the first of the numbers
// the journal-only files use.
constexpr ULONGLONG ghost_record = 40;
constexpr ULONGLONG first_journal_only_record = 41;

// Sequence number of the live report file in the fixture.
constexpr WORD report_sequence = 3;

// Times the records carry, increasing.
constexpr ULONGLONG time_a = 0x01D87F3C00000100ULL;
constexpr ULONGLONG time_b = 0x01D87F3C00000200ULL;
constexpr ULONGLONG time_c = 0x01D87F3C00000300ULL;

// A V2 record of a file named by record and sequence, filed under parent.
FakeUsnRecord Rec(std::wstring name, ULONGLONG record, WORD sequence,
                  DWORD reason, ULONGLONG time,
                  ULONGLONG parent = Tests::mft_tree_docs_idx,
                  WORD parent_sequence = 1) {
  return FakeUsnRecord{.name = std::move(name),
                       .file_record = record,
                       .file_sequence = sequence,
                       .parent_record = parent,
                       .parent_sequence = parent_sequence,
                       .time = time,
                       .reason = reason,
                       .file_attributes = attribute_archive};
}

// Records that name files the tree has: a live one touched three times, a
// freed one that was deleted, a rename chain of a file no record holds, the
// sequence rules, and a reference with a high half.
FakeUsnOptions Annotated() {
  FakeUsnRecord wide = Rec(L"wide", Tests::mft_tree_report_idx, report_sequence,
                           reason_file_create, time_a);
  wide.major_version = 3;
  wide.file_reference_high = 1;
  return {.records = {
              Rec(L"report.txt", Tests::mft_tree_report_idx, report_sequence,
                  reason_file_create, time_a),
              Rec(L"report.txt", Tests::mft_tree_report_idx, report_sequence,
                  reason_data_overwrite, time_b),
              Rec(L"report.txt", Tests::mft_tree_report_idx, report_sequence,
                  reason_data_overwrite | reason_close, time_b),
              Rec(L"old.tmp", Tests::mft_tree_deleted_file_idx, 1,
                  reason_file_create, time_a),
              Rec(L"old.tmp", Tests::mft_tree_deleted_file_idx, 1,
                  reason_file_delete | reason_close, time_c),
              Rec(L"a.txt", ghost_record, 2, reason_file_create, time_a),
              Rec(L"a.txt", ghost_record, 2, reason_rename_old, time_b),
              Rec(L"b.txt", ghost_record, 2, reason_rename_new, time_b),
              Rec(L"c.txt", ghost_record, 2, reason_rename_new | reason_close,
                  time_c),
              Rec(L"zero", Tests::mft_tree_hard_link_idx, 0, reason_file_create,
                  time_a),
              Rec(L"older", Tests::mft_tree_report_idx, report_sequence - 1,
                  reason_file_create, time_b),
              Rec(L"newer", Tests::mft_tree_report_idx, report_sequence + 1,
                  reason_file_create, time_c),
              wide}};
}

// Files only the journal names, each under another kind of parent: a live
// directory, the root, a record that does not exist, a freed directory and a
// plain file. One of them is a directory itself.
FakeUsnOptions JournalOnlyPaths() {
  const ULONGLONG first = first_journal_only_record;
  FakeUsnRecord folder =
      Rec(L"folder", first + 4, 1, reason_file_create, time_a);
  folder.file_attributes = attribute_directory;
  return {
      .records = {
          Rec(L"in-docs.bin", first, 1, reason_file_create, time_a),
          Rec(L"in-root.bin", first + 1, 1, reason_file_create, time_a, 5, 5),
          Rec(L"in-lost.bin", first + 2, 1, reason_file_create, time_a, 99, 1),
          Rec(L"in-old-dir.bin", first + 3, 1, reason_file_create, time_a,
              Tests::mft_tree_deleted_dir_idx, 3),
          Rec(L"in-file.bin", first + 5, 1, reason_file_create, time_a,
              Tests::mft_tree_report_idx, report_sequence),
          std::move(folder)}};
}

// A short journal, for the variants that break the way to it.
FakeUsnOptions ShortJournal() {
  return {.records = {Rec(L"report.txt", Tests::mft_tree_report_idx,
                          report_sequence, reason_file_create, time_a),
                      Rec(L"ghost.bin", ghost_record, 2,
                          reason_file_delete | reason_close, time_c)}};
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
      {.hole_bytes = Tests::fake_cluster_size,
       .records = {Rec(L"later.txt", ghost_record + 1, 1, reason_file_create,
                       time_b)}});
  cut.cut_run_list = true;
  variants.push_back({"usn_loader_cut_run_list", std::move(cut)});
  return variants;
}

// Where the name of a V2 record starts: right after its fixed part.
constexpr size_t v2_name_offset = 60;

// The stream the journal scanner meets as a file of its own: a first sector
// that the boot sector patches wreck, then records at their own offsets, two of
// them with a name that is not UTF-16, and stray bytes after the last.
std::vector<BYTE> ScannerStream() {
  constexpr size_t damaged_head = 512;
  constexpr WORD lone_high_surrogate = 0xD800;
  constexpr WORD lone_low_surrogate = 0xDC00;
  constexpr BYTE stray = 0x5A;
  constexpr size_t stray_bytes = 3;

  std::vector<BYTE> stream(damaged_head, 0);
  const auto append = [&stream](const FakeUsnRecord& record, WORD unit) {
    std::vector<BYTE> bytes = Tests::EncodeUsnRecord(record, stream.size());
    if (unit != 0) {
      std::memcpy(&bytes.at(v2_name_offset), &unit, sizeof(unit));
    }
    stream.insert(stream.end(), bytes.begin(), bytes.end());
  };
  append(Rec(L"high0", 100, 1, reason_file_create, time_a),
         lone_high_surrogate);
  append(Rec(L"low00", 101, 1, reason_file_create, time_a), lone_low_surrogate);
  append(Rec(L"fine0", 102, 1, reason_file_create, time_a), 0);
  stream.insert(stream.end(), stray_bytes, stray);
  return stream;
}

}  // namespace

// Writes the saved fuzz inputs where a change journal meets the tree. The
// volume is opened recovering with deleted items shown, since only then does a
// scan read the journal and list the files only it names. usn_scanner_* is not
// recorded from an image: the fuzzer feeds every input to the journal scanner
// as well, and that file is the stream it finds. Set
// NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the repository's
// NTFSLibTests/fuzz/data) and run this hidden test case by its tag.
TEST_CASE("write the usn tree fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (const Variant& variant : Variants()) {
    std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
    Tests::PutUsnJournal(image, variant.options);
    Tests::FuzzInputs::WriteRecordedInput(std::filesystem::path(dir) /
                                              variant.name,
                                          std::move(image), recovering);
  }

  const std::vector<BYTE> scanner = ScannerStream();
  std::ofstream out(std::filesystem::path(dir) / "usn_scanner_damaged_names",
                    std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(scanner.data()),
            static_cast<std::streamsize>(scanner.size()));
}
