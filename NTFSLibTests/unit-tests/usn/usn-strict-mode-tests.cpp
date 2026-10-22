#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::FakeUsnRecord;

namespace {

template <Cache::Strategy S>
void CheckStrictModeReadsNothing(const VolumeOptions& options) {
  const auto opened = Support::Open<S>(
      Support::JournalImage({.records = {FakeUsnRecord{.name = L"a.txt"}}}),
      options);
  opened.counter->Reset();

  CHECK(opened.volume->GetUsnJournal() == nullptr);

  CHECK(opened.counter->ReadCount() == 0);
  CHECK(opened.counter->Calls().empty());
}

template <Cache::Strategy S>
void CheckRecoveryModeReadsTheJournal() {
  const auto opened = Support::Open<S>(
      Support::JournalImage({.records = {FakeUsnRecord{.name = L"a.txt"}}}));
  opened.counter->Reset();

  CHECK(opened.volume->GetUsnJournal() != nullptr);

  CHECK(opened.counter->ReadCount() > 0);
}

}  // namespace

TEST_CASE("strict mode never reads the journal", "[usn][strict]") {
  CheckStrictModeReadsNothing<Cache::Strategy::NoCache>(VolumeOptions{});
  CheckStrictModeReadsNothing<Cache::Strategy::FullCache>(VolumeOptions{});
}

TEST_CASE("include_deleted alone does not read the journal", "[usn][strict]") {
  const VolumeOptions options{.include_deleted = true};
  CheckStrictModeReadsNothing<Cache::Strategy::NoCache>(options);
  CheckStrictModeReadsNothing<Cache::Strategy::FullCache>(options);
}

TEST_CASE("recovery mode reads the journal", "[usn][strict]") {
  CheckRecoveryModeReadsTheJournal<Cache::Strategy::NoCache>();
  CheckRecoveryModeReadsTheJournal<Cache::Strategy::FullCache>();
}
