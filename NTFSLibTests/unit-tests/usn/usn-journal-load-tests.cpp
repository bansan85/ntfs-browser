#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
#include "scripted-disk-reader.h"
#include "usn-test-support.h"
#include "usn/usn-journal-loader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
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

namespace {

template <Cache::Strategy S>
void CheckADamagedRunListIsReadAsFarAsItGoes() {
  const auto sink = std::make_shared<AllocSupport::LevelSink>();
  const AllocSupport::ScopedSink scoped(sink);
  // The cut drops the run of the second stretch, which follows a hole.
  const auto opened = Support::Open<S>(Support::JournalImage(
      {.records = {Ghost()},
       .more_stretches = {{.hole_bytes = Tests::fake_cluster_size,
                           .records = {{.name = L"far.txt",
                                        .file_record = 41}}}},
       .cut_run_list = true}));

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  CHECK_FALSE(journal->records.empty());
  CHECK(sink->Count("the run list of $J is damaged") == 1);
  CHECK(sink->LevelOf("the run list of $J is damaged") == Log::Level::Info);
}

template <Cache::Strategy S>
void CheckStrictModeLoadsNothing() {
  const auto opened = Support::Open<S>(
      Support::JournalImage({.records = {Ghost()}}), VolumeOptions{});

  CHECK(Usn::LoadJournal(*opened.volume) == nullptr);
}

// The journal of image over a reader that throws on every read of its data.
template <Cache::Strategy S>
void CheckAThrowingReaderGivesNoJournal() {
  const std::vector<BYTE> base = Tests::BuildFakeNtfsImageWithFreedFileTree();
  std::vector<BYTE> image = base;
  Tests::PutUsnJournal(image, {.records = {Ghost()}});
  const ULONGLONG data_start = ((base.size() + Tests::fake_cluster_size - 1) /
                                Tests::fake_cluster_size) *
                               Tests::fake_cluster_size;
  const ULONGLONG image_size = image.size();

  auto reader = std::make_unique<Tests::ScriptedDiskReader>(std::move(image));
  reader->FaultRange(data_start, image_size,
                     Tests::ScriptedDiskReader::Fault::Throw);
  const NtfsVolume<S> volume(std::move(reader), Support::RecoverOptions());
  REQUIRE(volume.IsVolumeOK());

  CHECK_NOTHROW(volume.GetUsnJournal());
  CHECK(volume.GetUsnJournal() == nullptr);
}

}  // namespace

TEST_CASE("a journal whose run list is damaged is read as far as it goes",
          "[usn][journal]") {
  CheckADamagedRunListIsReadAsFarAsItGoes<Cache::Strategy::NoCache>();
  CheckADamagedRunListIsReadAsFarAsItGoes<Cache::Strategy::FullCache>();
}

TEST_CASE("LoadJournal gives nothing to a volume that does not recover",
          "[usn][journal]") {
  CheckStrictModeLoadsNothing<Cache::Strategy::NoCache>();
  CheckStrictModeLoadsNothing<Cache::Strategy::FullCache>();
}

TEST_CASE("a reader that throws while the journal is read gives no journal",
          "[usn][journal]") {
  CheckAThrowingReaderGivesNoJournal<Cache::Strategy::NoCache>();
  CheckAThrowingReaderGivesNoJournal<Cache::Strategy::FullCache>();
}
