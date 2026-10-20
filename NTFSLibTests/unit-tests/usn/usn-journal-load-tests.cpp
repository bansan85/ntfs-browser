#include <ntfs-browser/win-types.h>

#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-failure-support.h"
#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using Tests::FakeUsnOptions;
using Tests::FakeUsnRecord;

namespace {

// USN_REASON_FILE_DELETE | USN_REASON_CLOSE.
constexpr DWORD delete_and_close = 0x80000200;

// The message of a journal that cannot be used.
constexpr std::string_view unusable_message = "$UsnJrnl is not usable";

// A file no record of the volume names: its slot is past $MFT.
FakeUsnRecord Ghost() {
  return FakeUsnRecord{.name = L"ghost.bin",
                       .file_record = 40,
                       .file_sequence = 2,
                       .parent_record = Tests::mft_tree_docs_idx,
                       .parent_sequence = 1,
                       .time = 0x01D87F3C0A0B0C0DULL,
                       .reason = delete_and_close,
                       .file_attributes = 0x20};
}

template <Cache::Strategy S>
void CheckAFileLostFromTheMftComesBack() {
  const auto opened = Support::Open<S>(Support::JournalImage(
      {.records = {Ghost(), {.name = L"second.txt", .file_record = 41}}}));

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  REQUIRE(journal->records.size() == 2);
  const Usn::Record& ghost = journal->records.at(0);
  CHECK(ghost.name == L"ghost.bin");
  CHECK(ghost.file_record == 40);
  CHECK(ghost.file_sequence == 2);
  CHECK(ghost.parent_record == Tests::mft_tree_docs_idx);
  CHECK(ghost.parent_sequence == 1);
  CHECK(ghost.reason == delete_and_close);
  CHECK(ghost.file_attributes == 0x20);
  CHECK(ghost.usn == 0);
  const FILETIME time = Support::ExpectedTime(0x01D87F3C0A0B0C0DULL);
  CHECK(ghost.time.dwLowDateTime == time.dwLowDateTime);
  CHECK(ghost.time.dwHighDateTime == time.dwHighDateTime);

  const Usn::Record& second = journal->records.at(1);
  CHECK(second.name == L"second.txt");
  CHECK(second.usn ==
        static_cast<LONGLONG>(Tests::EncodeUsnRecord(Ghost(), 0).size()));
  CHECK(journal->damaged_records == 0);
  CHECK_FALSE(journal->truncated);
}

template <Cache::Strategy S>
void CheckTheResultIsOwnedData() {
  const auto opened = Support::Open<S>(
      Support::JournalImage({.records = {Ghost(), {.name = L"two.txt"}}}));
  const Usn::Journal* journal = opened.volume->GetUsnJournal();
  REQUIRE(journal != nullptr);
  REQUIRE(journal->records.size() == 2);
  const std::vector<Usn::Record> before = journal->records;

  // Another read replaces a NoCache buffer, and MinifyCache() frees every
  // FullCache block no view holds.
  opened.volume->MinifyCache();
  FileRecord<S> record(*opened.volume);
  REQUIRE(record.ParseFileRecord(Tests::mft_tree_docs_idx));
  REQUIRE(record.ParseAttrs());
  opened.volume->MinifyCache();

  REQUIRE(opened.volume->GetUsnJournal() == journal);
  REQUIRE(journal->records.size() == before.size());
  for (size_t i = 0; i < before.size(); ++i) {
    CHECK(journal->records.at(i).name == before.at(i).name);
    CHECK(journal->records.at(i).usn == before.at(i).usn);
    CHECK(journal->records.at(i).file_record == before.at(i).file_record);
  }
}

template <Cache::Strategy S>
void CheckAFreedJournalRecordIsStillRead() {
  const auto opened = Support::Open<S>(
      Support::JournalImage(
          {.records = {Ghost()}, .journal_record_freed = true}),
      VolumeOptions{.include_deleted = false, .recover_errors = true});

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  CHECK(journal->records.size() == 1);
}

template <Cache::Strategy S>
void CheckAnAbsentJournalIsNull(const FakeUsnOptions& options) {
  const auto sink = std::make_shared<AllocSupport::LevelSink>();
  const AllocSupport::ScopedSink scoped(sink);
  const auto opened = Support::Open<S>(Support::JournalImage(options));

  CHECK(opened.volume->GetUsnJournal() == nullptr);

  // A volume with no journal is normal: nothing above Info.
  CHECK(sink->Count(unusable_message) == 1);
  CHECK(sink->LevelOf(unusable_message) == Log::Level::Info);
  for (const AllocSupport::LevelSink::Line& line : sink->lines) {
    CHECK(line.level != Log::Level::Warn);
    CHECK(line.level != Log::Level::Error);
  }
}

template <Cache::Strategy S>
void CheckAJournalWithNoRecordIsEmpty() {
  const auto opened = Support::Open<S>(Support::JournalImage({}));

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  CHECK(journal->records.empty());
}

}  // namespace

TEST_CASE("a file the MFT lost comes back from the journal", "[usn][journal]") {
  CheckAFileLostFromTheMftComesBack<Cache::Strategy::NoCache>();
  CheckAFileLostFromTheMftComesBack<Cache::Strategy::FullCache>();
}

TEST_CASE("the journal is owned data that later reads do not disturb",
          "[usn][journal]") {
  CheckTheResultIsOwnedData<Cache::Strategy::NoCache>();
  CheckTheResultIsOwnedData<Cache::Strategy::FullCache>();
}

TEST_CASE("a freed $UsnJrnl record is read whatever include_deleted says",
          "[usn][journal]") {
  CheckAFreedJournalRecordIsStillRead<Cache::Strategy::NoCache>();
  CheckAFreedJournalRecordIsStillRead<Cache::Strategy::FullCache>();
}

TEST_CASE("a volume with no $UsnJrnl entry has no journal", "[usn][journal]") {
  CheckAnAbsentJournalIsNull<Cache::Strategy::NoCache>(
      {.records = {Ghost()}, .omit_extend_entry = true});
  CheckAnAbsentJournalIsNull<Cache::Strategy::FullCache>(
      {.records = {Ghost()}, .omit_extend_entry = true});
}

TEST_CASE("a $UsnJrnl with no $J stream has no journal", "[usn][journal]") {
  CheckAnAbsentJournalIsNull<Cache::Strategy::NoCache>(
      {.records = {Ghost()}, .omit_j_stream = true});
  CheckAnAbsentJournalIsNull<Cache::Strategy::FullCache>(
      {.records = {Ghost()}, .omit_j_stream = true});
}

TEST_CASE("a $UsnJrnl record that does not parse has no journal",
          "[usn][journal]") {
  CheckAnAbsentJournalIsNull<Cache::Strategy::NoCache>(
      {.records = {Ghost()}, .omit_journal_record = true});
  CheckAnAbsentJournalIsNull<Cache::Strategy::FullCache>(
      {.records = {Ghost()}, .omit_journal_record = true});
}

TEST_CASE("a journal that holds no record is empty, not absent",
          "[usn][journal]") {
  CheckAJournalWithNoRecordIsEmpty<Cache::Strategy::NoCache>();
  CheckAJournalWithNoRecordIsEmpty<Cache::Strategy::FullCache>();
}
