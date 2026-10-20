#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeUsnOptions;
using Tests::FakeUsnRecord;

namespace {

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

// Record numbers of the fixture: report.txt is live, old.tmp is freed, and no
// record names the ghost.
constexpr ULONGLONG report_record = Tests::mft_tree_report_idx;
constexpr WORD report_sequence = 3;
constexpr ULONGLONG deleted_record = Tests::mft_tree_deleted_file_idx;
constexpr ULONGLONG link_record = Tests::mft_tree_hard_link_idx;
constexpr ULONGLONG ghost_record = 40;

// Times the records of a fixture carry, increasing.
constexpr ULONGLONG time_a = 0x01D87F3C00000100ULL;
constexpr ULONGLONG time_b = 0x01D87F3C00000200ULL;
constexpr ULONGLONG time_c = 0x01D87F3C00000300ULL;

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

// The tree of a scan over the fixture with the journal in options.
template <Cache::Strategy S>
Mft::MftTree ScanWith(const FakeUsnOptions& options,
                      const VolumeOptions& volume_options,
                      const Mft::MftTree::ScanOptions& scan = {}) {
  const auto opened =
      Support::Open<S>(Support::JournalImage(options), volume_options);
  return Mft::MftTree(*opened.volume, scan);
}

bool SameTime(const FILETIME& time, ULONGLONG utc) {
  const FILETIME expected = Support::ExpectedTime(utc);
  return time.dwLowDateTime == expected.dwLowDateTime &&
         time.dwHighDateTime == expected.dwHighDateTime;
}

const Mft::MftTree::JournalOnlyEntry*
    FindJournalOnly(const Mft::MftTree& tree, ULONGLONG record, WORD sequence) {
  for (const Mft::MftTree::JournalOnlyEntry& item : tree.JournalOnly()) {
    if (item.record == record && item.sequence == sequence) {
      return &item;
    }
  }
  return nullptr;
}

// A journal that touches a live file, a freed one, and one no record names.
FakeUsnOptions MixedJournal() {
  return {.records = {Rec(L"report.txt", report_record, report_sequence,
                          reason_file_create, time_a),
                      Rec(L"old.tmp", deleted_record, 1,
                          reason_file_delete | reason_close, time_b),
                      Rec(L"ghost.bin", ghost_record, 2,
                          reason_file_delete | reason_close, time_c)}};
}

template <Cache::Strategy S>
void CheckTheFourFlagCombinations() {
  SECTION("strict mode: nothing from the journal") {
    const Mft::MftTree tree =
        ScanWith<S>(MixedJournal(), VolumeOptions{.include_deleted = false});
    CHECK(tree.JournalOnly().empty());
    for (const Mft::MftTree::Entry& entry : tree.Entries()) {
      CHECK_FALSE(entry.usn);
    }
    CHECK(tree.Stats().usn_records == 0);
    CHECK(tree.Stats().usn_annotated == 0);
    CHECK(tree.Stats().usn_journal_only == 0);
  }

  SECTION("deleted items alone: nothing from the journal") {
    const Mft::MftTree tree =
        ScanWith<S>(MixedJournal(), VolumeOptions{.include_deleted = true});
    CHECK(tree.JournalOnly().empty());
    for (const Mft::MftTree::Entry& entry : tree.Entries()) {
      CHECK_FALSE(entry.usn);
    }
    CHECK(tree.Stats().usn_records == 0);
  }

  SECTION("recovery alone: annotates, lists no journal-only file") {
    const Mft::MftTree tree =
        ScanWith<S>(MixedJournal(), VolumeOptions{.include_deleted = false,
                                                  .recover_errors = true});
    const Mft::MftTree::Entry* report = tree.Find(report_record);
    REQUIRE(report != nullptr);
    CHECK(report->usn);
    CHECK(tree.JournalOnly().empty());
    CHECK(tree.Stats().usn_records == 3);
    CHECK(tree.Stats().usn_annotated == 1);
    CHECK(tree.Stats().usn_journal_only == 0);
  }

  SECTION("recovery and deleted items: annotates and lists") {
    const Mft::MftTree tree =
        ScanWith<S>(MixedJournal(), Support::RecoverOptions());
    const Mft::MftTree::Entry* report = tree.Find(report_record);
    const Mft::MftTree::Entry* deleted = tree.Find(deleted_record);
    REQUIRE(report != nullptr);
    REQUIRE(deleted != nullptr);
    CHECK(report->usn);
    CHECK(deleted->usn);
    REQUIRE(tree.JournalOnly().size() == 1);
    CHECK(tree.JournalOnly().front().record == ghost_record);
    CHECK(tree.Stats().usn_records == 3);
    CHECK(tree.Stats().usn_annotated == 2);
    CHECK(tree.Stats().usn_journal_only == 1);
    CHECK(tree.Stats().usn_damaged == 0);
    CHECK_FALSE(tree.Stats().usn_truncated);
  }
}

template <Cache::Strategy S>
void CheckUsnInfoFields() {
  const Mft::MftTree tree = ScanWith<S>(
      {.records = {Rec(L"report.txt", report_record, report_sequence,
                       reason_file_create, time_a),
                   Rec(L"report.txt", report_record, report_sequence,
                       reason_data_overwrite, time_b),
                   Rec(L"report.txt", report_record, report_sequence,
                       reason_data_overwrite | reason_close, time_c)}},
      Support::RecoverOptions());

  const Mft::MftTree::Entry* report = tree.Find(report_record);
  REQUIRE(report != nullptr);
  REQUIRE(report->usn);
  const Mft::MftTree::UsnInfo& info = *report->usn;
  CHECK(info.record_count == 3);
  CHECK(info.last_reason == (reason_data_overwrite | reason_close));
  CHECK(info.all_reasons ==
        (reason_file_create | reason_data_overwrite | reason_close));
  CHECK(SameTime(info.last_time, time_c));
  CHECK(info.last_usn > 0);
  CHECK_FALSE(info.delete_time);
}

template <Cache::Strategy S>
void CheckTheDeletionTimeOfAFreedFile() {
  // old.tmp is freed under sequence 2: a journal record written while it lived
  // carries sequence 1, the one NTFS bumped on deletion.
  const Mft::MftTree tree =
      ScanWith<S>({.records = {Rec(L"old.tmp", deleted_record, 1,
                                   reason_file_create, time_a),
                               Rec(L"old.tmp", deleted_record, 1,
                                   reason_file_delete | reason_close, time_b)}},
                  Support::RecoverOptions());

  const Mft::MftTree::Entry* deleted = tree.Find(deleted_record);
  REQUIRE(deleted != nullptr);
  REQUIRE(deleted->usn);
  REQUIRE(deleted->usn->delete_time);
  CHECK(SameTime(*deleted->usn->delete_time, time_b));
  CHECK(tree.JournalOnly().empty());
}

template <Cache::Strategy S>
void CheckTheSequenceRules() {
  // In use under sequence 3: sequence 0 claims nothing, 3 is the generation, 2
  // is a file that lived there before, and 4 would be the freed generation,
  // which a live record MUST NOT take.
  const Mft::MftTree tree = ScanWith<S>(
      {.records = {Rec(L"zero", link_record, 0, reason_file_create, time_a),
                   Rec(L"same", report_record, report_sequence,
                       reason_file_create, time_a),
                   Rec(L"older", report_record, report_sequence - 1,
                       reason_file_create, time_b),
                   Rec(L"newer", report_record, report_sequence + 1,
                       reason_file_create, time_c)}},
      Support::RecoverOptions());

  const Mft::MftTree::Entry* linked = tree.Find(link_record);
  const Mft::MftTree::Entry* report = tree.Find(report_record);
  REQUIRE(linked != nullptr);
  REQUIRE(report != nullptr);
  CHECK(linked->usn);
  REQUIRE(report->usn);
  CHECK(report->usn->record_count == 1);

  CHECK(FindJournalOnly(tree, report_record, report_sequence - 1) != nullptr);
  CHECK(FindJournalOnly(tree, report_record, report_sequence + 1) != nullptr);
  CHECK(FindJournalOnly(tree, report_record, report_sequence) == nullptr);
  CHECK(FindJournalOnly(tree, link_record, 0) == nullptr);
}

template <Cache::Strategy S>
void CheckTheNewestNameWins() {
  const Mft::MftTree tree = ScanWith<S>(
      {.records = {Rec(L"a.txt", ghost_record, 2, reason_file_create, time_a),
                   Rec(L"a.txt", ghost_record, 2, reason_rename_old, time_b),
                   Rec(L"b.txt", ghost_record, 2, reason_rename_new, time_b),
                   Rec(L"b.txt", ghost_record, 2, reason_rename_old, time_c),
                   Rec(L"c.txt", ghost_record, 2,
                       reason_rename_new | reason_close, time_c)}},
      Support::RecoverOptions());

  const Mft::MftTree::JournalOnlyEntry* item =
      FindJournalOnly(tree, ghost_record, 2);
  REQUIRE(item != nullptr);
  CHECK(item->name == L"c.txt");
  CHECK(item->usn.record_count == 5);
  CHECK(item->usn.all_reasons == (reason_file_create | reason_rename_old |
                                  reason_rename_new | reason_close));
  CHECK(SameTime(item->usn.last_time, time_c));
}

template <Cache::Strategy S>
void CheckJournalOnlyPaths() {
  FakeUsnRecord in_docs =
      Rec(L"in-docs.bin", 41, 1, reason_file_create, time_a);
  FakeUsnRecord in_root =
      Rec(L"in-root.bin", 42, 1, reason_file_create, time_a, 5, 5);
  FakeUsnRecord in_lost =
      Rec(L"in-lost.bin", 43, 1, reason_file_create, time_a, 99, 1);
  FakeUsnRecord in_freed_dir = Rec(L"in-old-dir.bin", 44, 1, reason_file_create,
                                   time_a, Tests::mft_tree_deleted_dir_idx, 3);
  FakeUsnRecord folder = Rec(L"folder", 45, 1, reason_file_create, time_a);
  folder.file_attributes = attribute_directory;

  const Mft::MftTree tree = ScanWith<S>(
      {.records = {in_docs, in_root, in_lost, in_freed_dir, folder}},
      Support::RecoverOptions());

  REQUIRE(tree.JournalOnly().size() == 5);
  const auto* docs_item = FindJournalOnly(tree, 41, 1);
  const auto* root_item = FindJournalOnly(tree, 42, 1);
  const auto* lost_item = FindJournalOnly(tree, 43, 1);
  const auto* freed_item = FindJournalOnly(tree, 44, 1);
  const auto* folder_item = FindJournalOnly(tree, 45, 1);
  REQUIRE(docs_item != nullptr);
  REQUIRE(root_item != nullptr);
  REQUIRE(lost_item != nullptr);
  REQUIRE(freed_item != nullptr);
  REQUIRE(folder_item != nullptr);

  CHECK(docs_item->parent_valid);
  CHECK(root_item->parent_valid);
  CHECK_FALSE(lost_item->parent_valid);
  CHECK(freed_item->parent_valid);
  CHECK(folder_item->directory);
  CHECK_FALSE(docs_item->directory);

  const auto index_of = [&](const Mft::MftTree::JournalOnlyEntry* item) {
    return static_cast<size_t>(item - tree.JournalOnly().data());
  };
  std::optional<ULONGLONG> lost;
  CHECK(tree.GetJournalPath(index_of(docs_item), &lost) ==
        L"\\Docs\\in-docs.bin");
  CHECK_FALSE(lost);
  CHECK(tree.GetJournalPath(index_of(root_item), &lost) == L"\\in-root.bin");
  CHECK_FALSE(lost);
  CHECK(tree.GetJournalPath(index_of(lost_item), &lost) == L"in-lost.bin");
  REQUIRE(lost);
  CHECK(*lost == 99);
  CHECK(tree.GetJournalPath(index_of(freed_item), &lost) ==
        L"\\Docs\\OldDir\\in-old-dir.bin");
  CHECK_FALSE(lost);
  CHECK(tree.GetJournalPath(tree.JournalOnly().size(), &lost).empty());
  CHECK_FALSE(lost);
}

template <Cache::Strategy S>
void CheckJournalOnlyFilesStayOutOfTheEntries() {
  const Mft::MftTree with_journal =
      ScanWith<S>(MixedJournal(), Support::RecoverOptions());
  REQUIRE(with_journal.JournalOnly().size() == 1);

  CHECK(with_journal.Find(ghost_record) == nullptr);
  CHECK_FALSE(with_journal.IsReachable(ghost_record));
  CHECK(with_journal.Children(Tests::mft_tree_docs_idx).size() ==
        with_journal.Children(Tests::mft_tree_docs_idx).size());
  for (const ULONGLONG child :
       with_journal.Children(Tests::mft_tree_docs_idx)) {
    CHECK(child != ghost_record);
  }
  for (const Mft::MftTree::Entry& entry : with_journal.Entries()) {
    CHECK(entry.record != ghost_record);
  }
}

template <Cache::Strategy S>
void CheckStrictModeIsTheScanWithoutAJournal() {
  const VolumeOptions strict{.include_deleted = true};
  const auto plain =
      Support::Open<S>(Tests::BuildFakeNtfsImageWithFreedFileTree(), strict);
  const Mft::MftTree without(*plain.volume);
  const Mft::MftTree with = ScanWith<S>(MixedJournal(), strict);

  // The image with a journal also holds the two records of the journal's own
  // files, which the other one leaves empty: set them aside.
  std::vector<const Mft::MftTree::Entry*> kept;
  for (const Mft::MftTree::Entry& entry : with.Entries()) {
    if (entry.record != static_cast<ULONGLONG>(Mft::Idx::Extend) &&
        entry.record != Tests::usn_journal_idx) {
      kept.push_back(&entry);
    }
  }
  REQUIRE(kept.size() == without.Entries().size());
  for (size_t i = 0; i < kept.size(); ++i) {
    const Mft::MftTree::Entry& a = *kept.at(i);
    const Mft::MftTree::Entry& b = without.Entries().at(i);
    CHECK(a.record == b.record);
    CHECK(a.sequence == b.sequence);
    CHECK(a.in_use == b.in_use);
    CHECK(a.names.size() == b.names.size());
    CHECK(a.size == b.size);
    CHECK(a.data_state == b.data_state);
    CHECK_FALSE(a.usn);
  }
  CHECK(with.Stats().slots == without.Stats().slots);
  CHECK(with.Stats().in_use == without.Stats().in_use + 2);
  CHECK(with.Stats().deleted == without.Stats().deleted);
  CHECK(with.Stats().unreadable + 2 == without.Stats().unreadable);
  CHECK(with.Stats().damaged == without.Stats().damaged);
  CHECK(with.Stats().unreachable == without.Stats().unreachable);
  CHECK(with.JournalOnly().empty());
}

template <Cache::Strategy S>
void CheckACopyKeepsTheJournalOnlyFiles() {
  Mft::MftTree original =
      ScanWith<S>(MixedJournal(), Support::RecoverOptions());
  const Mft::MftTree copy(original);
  const Mft::MftTree moved(std::move(original));

  REQUIRE(copy.JournalOnly().size() == 1);
  CHECK(copy.JournalOnly().front().name == L"ghost.bin");
  REQUIRE(moved.JournalOnly().size() == 1);
  CHECK(moved.JournalOnly().front().record == ghost_record);
}

template <Cache::Strategy S>
void CheckAHighReferenceNeverMatches() {
  FakeUsnRecord wide =
      Rec(L"wide", report_record, report_sequence, reason_file_create, time_a);
  wide.major_version = 3;
  wide.file_reference_high = 1;

  const Mft::MftTree tree =
      ScanWith<S>({.records = {wide}}, Support::RecoverOptions());

  const Mft::MftTree::Entry* report = tree.Find(report_record);
  REQUIRE(report != nullptr);
  CHECK_FALSE(report->usn);
  CHECK(tree.JournalOnly().size() == 1);
}

template <Cache::Strategy S>
void CheckJournalOrderBreaksATimeTie() {
  const Mft::MftTree tree = ScanWith<S>(
      {.records = {Rec(L"report.txt", report_record, report_sequence,
                       reason_data_overwrite, time_a),
                   Rec(L"report.txt", report_record, report_sequence,
                       reason_close, time_a)}},
      Support::RecoverOptions());

  const Mft::MftTree::Entry* report = tree.Find(report_record);
  REQUIRE(report != nullptr);
  REQUIRE(report->usn);
  CHECK(report->usn->last_reason == reason_close);
}

template <Cache::Strategy S>
void CheckADamagedRecordDoesNotStopTheScan() {
  const std::vector<BYTE> first =
      Tests::EncodeUsnRecord(Rec(L"report.txt", report_record, report_sequence,
                                 reason_file_create, time_a),
                             0);
  // The second record's length is forged: the scan skips it and finds the
  // third.
  FakeUsnOptions options = MixedJournal();
  options.patches.push_back(
      {.stream_offset = first.size(), .bytes = {0x3E, 0x00, 0x00, 0x00}});
  const Mft::MftTree tree = ScanWith<S>(options, Support::RecoverOptions());

  const Mft::MftTree::Entry* report = tree.Find(report_record);
  REQUIRE(report != nullptr);
  CHECK(report->usn);
  CHECK(tree.Stats().usn_damaged >= 1);
  CHECK(FindJournalOnly(tree, ghost_record, 2) != nullptr);
}

template <Cache::Strategy S>
void CheckAStoppedScanLeavesTheJournalAlone() {
  Mft::MftTree::ScanOptions scan;
  scan.progress = [](ULONGLONG /*done*/, ULONGLONG /*total*/) { return false; };
  const Mft::MftTree tree =
      ScanWith<S>(MixedJournal(), Support::RecoverOptions(), scan);

  CHECK_FALSE(tree.Stats().complete);
  CHECK(tree.JournalOnly().empty());
  CHECK(tree.Stats().usn_records == 0);
}

}  // namespace

TEST_CASE("the flags decide what the journal gives a scan", "[usn][tree]") {
  CheckTheFourFlagCombinations<Cache::Strategy::NoCache>();
  CheckTheFourFlagCombinations<Cache::Strategy::FullCache>();
}

TEST_CASE("an entry reports the journal's last record and every reason",
          "[usn][tree]") {
  CheckUsnInfoFields<Cache::Strategy::NoCache>();
  CheckUsnInfoFields<Cache::Strategy::FullCache>();
}

TEST_CASE("a freed file gets its deletion time from the journal",
          "[usn][tree]") {
  CheckTheDeletionTimeOfAFreedFile<Cache::Strategy::NoCache>();
  CheckTheDeletionTimeOfAFreedFile<Cache::Strategy::FullCache>();
}

TEST_CASE("a journal record fits an entry only under its generation",
          "[usn][tree]") {
  CheckTheSequenceRules<Cache::Strategy::NoCache>();
  CheckTheSequenceRules<Cache::Strategy::FullCache>();
}

TEST_CASE("a file renamed twice takes its newest name", "[usn][tree]") {
  CheckTheNewestNameWins<Cache::Strategy::NoCache>();
  CheckTheNewestNameWins<Cache::Strategy::FullCache>();
}

TEST_CASE("a journal-only file is reached through a parent it can trust",
          "[usn][tree]") {
  CheckJournalOnlyPaths<Cache::Strategy::NoCache>();
  CheckJournalOnlyPaths<Cache::Strategy::FullCache>();
}

TEST_CASE("journal-only files are not entries of the tree", "[usn][tree]") {
  CheckJournalOnlyFilesStayOutOfTheEntries<Cache::Strategy::NoCache>();
  CheckJournalOnlyFilesStayOutOfTheEntries<Cache::Strategy::FullCache>();
}

TEST_CASE("in strict mode the tree is the one without a journal",
          "[usn][tree]") {
  CheckStrictModeIsTheScanWithoutAJournal<Cache::Strategy::NoCache>();
  CheckStrictModeIsTheScanWithoutAJournal<Cache::Strategy::FullCache>();
}

TEST_CASE("a copied tree keeps its journal-only files", "[usn][tree]") {
  CheckACopyKeepsTheJournalOnlyFiles<Cache::Strategy::NoCache>();
  CheckACopyKeepsTheJournalOnlyFiles<Cache::Strategy::FullCache>();
}

TEST_CASE("a reference with a high half never names an MFT record",
          "[usn][tree]") {
  CheckAHighReferenceNeverMatches<Cache::Strategy::NoCache>();
  CheckAHighReferenceNeverMatches<Cache::Strategy::FullCache>();
}

TEST_CASE("journal order, not time, decides the newest record", "[usn][tree]") {
  CheckJournalOrderBreaksATimeTie<Cache::Strategy::NoCache>();
  CheckJournalOrderBreaksATimeTie<Cache::Strategy::FullCache>();
}

TEST_CASE("a damaged journal record does not stop a scan", "[usn][tree]") {
  CheckADamagedRecordDoesNotStopTheScan<Cache::Strategy::NoCache>();
  CheckADamagedRecordDoesNotStopTheScan<Cache::Strategy::FullCache>();
}

TEST_CASE("a scan that progress() stopped does not read the journal",
          "[usn][tree]") {
  CheckAStoppedScanLeavesTheJournalAlone<Cache::Strategy::NoCache>();
  CheckAStoppedScanLeavesTheJournalAlone<Cache::Strategy::FullCache>();
}
