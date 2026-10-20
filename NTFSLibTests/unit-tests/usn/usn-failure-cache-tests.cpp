#include <ntfs-browser/win-types.h>

#include <memory>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>

#include "alloc-failure-support.h"
#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using Tests::FakeUsnRecord;

namespace {

// The message of a journal that cannot be used.
constexpr std::string_view unusable_message = "$UsnJrnl is not usable";

template <Cache::Strategy S>
void CheckAnUnreadableStreamIsNullAndNotRetried() {
  const auto sink = std::make_shared<AllocSupport::LevelSink>();
  const AllocSupport::ScopedSink scoped(sink);
  const auto opened = Support::Open<S>(Support::JournalImage(
      {.records = {FakeUsnRecord{.name = L"a.txt"}}, .unreadable_data = true}));

  REQUIRE(opened.volume->GetUsnJournal() == nullptr);
  CHECK(sink->Count(unusable_message) == 1);

  opened.counter->Reset();
  CHECK(opened.volume->GetUsnJournal() == nullptr);
  CHECK(opened.counter->ReadCount() == 0);
  CHECK(sink->Count(unusable_message) == 1);
}

template <Cache::Strategy S>
void CheckAMissingJournalIsNotLookedForAgain() {
  const auto opened = Support::Open<S>(
      Support::JournalImage({.records = {FakeUsnRecord{.name = L"a.txt"}},
                             .omit_extend_entry = true}));
  REQUIRE(opened.volume->GetUsnJournal() == nullptr);

  opened.counter->Reset();
  CHECK(opened.volume->GetUsnJournal() == nullptr);
  CHECK(opened.counter->ReadCount() == 0);
}

template <Cache::Strategy S>
void CheckTheVolumeKeepsWorkingAfterAFailure() {
  const auto opened = Support::Open<S>(Support::JournalImage(
      {.records = {FakeUsnRecord{.name = L"a.txt"}}, .unreadable_data = true}));
  REQUIRE(opened.volume->GetUsnJournal() == nullptr);

  FileRecord<S> record(*opened.volume);
  CHECK(record.ParseFileRecord(Tests::mft_tree_docs_idx));
  CHECK(record.ParseAttrs());
}

}  // namespace

TEST_CASE("a journal whose stream cannot be read is null and not read again",
          "[usn][failure]") {
  CheckAnUnreadableStreamIsNullAndNotRetried<Cache::Strategy::NoCache>();
  CheckAnUnreadableStreamIsNullAndNotRetried<Cache::Strategy::FullCache>();
}

TEST_CASE("a missing journal is not looked for again", "[usn][failure]") {
  CheckAMissingJournalIsNotLookedForAgain<Cache::Strategy::NoCache>();
  CheckAMissingJournalIsNotLookedForAgain<Cache::Strategy::FullCache>();
}

TEST_CASE("a journal failure leaves the volume usable", "[usn][failure]") {
  CheckTheVolumeKeepsWorkingAfterAFailure<Cache::Strategy::NoCache>();
  CheckTheVolumeKeepsWorkingAfterAFailure<Cache::Strategy::FullCache>();
}
