#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "catch2/matchers/catch_matchers.hpp"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeDataRun;

namespace {

// What the check says about a bad cluster, as a substring of its message.
constexpr const char* bad_message =
    "clusters of a data stream are marked bad in $BadClus";

// What the check of feature 005 says about a free cluster.
constexpr const char* free_message =
    "clusters of a data stream are free in $Bitmap";

// Bytes of the stale stream: four clusters at LCN 40.
constexpr ULONGLONG stale_size = 4 * Tests::fake_cluster_size;

const VolumeOptions strict{};
const VolumeOptions recovering{.recover_errors = true};

// One bad cluster inside the stale file's run.
const std::vector<Alloc::Extent> one_bad{{.lcn = 41, .clusters = 1}};

// A live stale file over four clusters, a bitmap that marks allocated_runs
// (and the bad clusters, as NTFS does, when marked is set), and a bad list.
std::vector<BYTE> LiveImage(std::span<const Alloc::Extent> bad,
                            std::span<const FakeDataRun> allocated_runs,
                            bool marked = true, bool in_use = true) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
      stale_size, {FakeDataRun{.lcn = 40, .clusters = 4}});
  for (size_t i = 0; i < stale_size; i++) {
    image.at(40 * Tests::fake_cluster_size + i) = static_cast<BYTE>(i * 13);
  }
  Tests::SetRecordInUse(image, Tests::mft_tree_stale_child_idx, in_use);
  std::vector<FakeDataRun> in_bitmap(allocated_runs.begin(),
                                     allocated_runs.end());
  if (marked) {
    in_bitmap = Support::WithBadRuns(std::move(in_bitmap), bad);
  }
  image = Support::WithBitmap(std::move(image), in_bitmap);
  return Support::WithBadClusters(std::move(image), bad);
}

// The other three clusters of the stale file, allocated in the bitmap.
const std::vector<FakeDataRun> rest_used{FakeDataRun{.lcn = 40, .clusters = 1},
                                         FakeDataRun{.lcn = 42, .clusters = 2}};

// Reads the whole stale stream.
template <Cache::Strategy S>
std::vector<BYTE> ReadStale(const Support::Opened<S>& opened) {
  const auto record =
      Support::ParsedRecord(*opened.volume, Tests::mft_tree_stale_child_idx);
  std::vector<BYTE> out(stale_size);
  const auto read = record->FindStream({})->ReadData(0, out);
  REQUIRE(read);
  REQUIRE(*read == stale_size);
  return out;
}

template <Cache::Strategy S>
void CheckABadClusterIsLogged() {
  const auto strict_opened =
      Support::Open<S>(LiveImage(one_bad, rest_used), strict);
  const std::vector<BYTE> expected = ReadStale(strict_opened);

  const auto opened =
      Support::Open<S>(LiveImage(one_bad, rest_used), recovering);
  (void)Tests::TakeCapturedLog();

  CHECK(ReadStale(opened) == expected);
  const std::string log = Tests::TakeCapturedLog();
  CHECK_THAT(log, ContainsSubstring(bad_message));
  CHECK_THAT(log, ContainsSubstring("1 clusters"));
  // The message names the start of the run that holds the bad cluster.
  CHECK_THAT(log, ContainsSubstring("LCN 40"));
}

template <Cache::Strategy S>
void CheckAPartlyBadRunIsLogged() {
  const std::vector<Alloc::Extent> two{{.lcn = 40, .clusters = 1},
                                       {.lcn = 43, .clusters = 1}};
  const std::array<FakeDataRun, 1> middle{
      FakeDataRun{.lcn = 41, .clusters = 2}};
  const auto opened = Support::Open<S>(LiveImage(two, middle), recovering);
  (void)Tests::TakeCapturedLog();

  (void)ReadStale(opened);

  const std::string log = Tests::TakeCapturedLog();
  CHECK_THAT(log, ContainsSubstring(bad_message));
  CHECK_THAT(log, ContainsSubstring("2 clusters"));
}

template <Cache::Strategy S>
void CheckAHealthyFileIsNotLogged() {
  const std::array<FakeDataRun, 1> all_used{
      FakeDataRun{.lcn = 40, .clusters = 4}};
  const auto opened = Support::Open<S>(LiveImage({}, all_used), recovering);
  (void)Tests::TakeCapturedLog();

  (void)ReadStale(opened);

  const std::string log = Tests::TakeCapturedLog();
  CHECK_THAT(log, !ContainsSubstring(bad_message));
  CHECK_THAT(log, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckStrictModeReadsNoBadList() {
  const std::vector<BYTE> image = LiveImage(one_bad, rest_used);
  const auto opened = Support::Open<S>(image, strict);
  (void)Tests::TakeCapturedLog();
  opened.counter->Reset();

  (void)ReadStale(opened);

  CHECK_THAT(Tests::TakeCapturedLog(), !ContainsSubstring(bad_message));
  CHECK(opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                     image.size()) == 0);
  // Reading the stale stream asks for no allocation at all: a failure to load
  // the bad list would be logged as soon as it was tried.
  CHECK_THAT(Tests::TakeCapturedLog(), !ContainsSubstring("$BadClus"));
}

template <Cache::Strategy S>
void CheckOneCauseRaisesOneMessage() {
  // The cluster is listed bad and its bit is clear: bad, never also free.
  const auto opened = Support::Open<S>(
      LiveImage(one_bad, rest_used, /*marked=*/false), recovering);
  (void)Tests::TakeCapturedLog();

  (void)ReadStale(opened);

  const std::string log = Tests::TakeCapturedLog();
  CHECK_THAT(log, ContainsSubstring(bad_message));
  CHECK_THAT(log, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckTheCheckRunsOnce() {
  const auto opened =
      Support::Open<S>(LiveImage(one_bad, rest_used), recovering);
  const auto record =
      Support::ParsedRecord(*opened.volume, Tests::mft_tree_stale_child_idx);
  std::vector<BYTE> out(stale_size);
  const auto* const stream = record->FindStream({});
  (void)Tests::TakeCapturedLog();

  REQUIRE(stream->ReadData(0, out));
  const std::string first = Tests::TakeCapturedLog();
  REQUIRE(stream->ReadData(0, out));
  const std::string second = Tests::TakeCapturedLog();

  CHECK_THAT(first, ContainsSubstring(bad_message));
  CHECK_THAT(second, !ContainsSubstring(bad_message));
}

template <Cache::Strategy S>
void CheckADeletedFileIsNotChecked() {
  const auto opened = Support::Open<S>(
      LiveImage(one_bad, rest_used, true, /*in_use=*/false),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  (void)Tests::TakeCapturedLog();

  (void)ReadStale(opened);

  CHECK_THAT(Tests::TakeCapturedLog(), !ContainsSubstring(bad_message));
}

template <Cache::Strategy S>
void CheckTheVolumesOwnMetadataIsNotChecked() {
  const std::vector<BYTE> image = LiveImage(one_bad, rest_used);
  const auto opened = Support::Open<S>(image, recovering);
  (void)Tests::TakeCapturedLog();

  const auto mft = Support::ParsedRecord(*opened.volume,
                                         static_cast<ULONGLONG>(Mft::Idx::Mft));
  std::vector<BYTE> mft_bytes(Tests::fake_cluster_size);
  CHECK(mft->FindStream({})->ReadData(0, mft_bytes));

  const auto bitmap = Support::ParsedRecord(
      *opened.volume, static_cast<ULONGLONG>(Mft::Idx::Bitmap));
  std::vector<BYTE> bitmap_bytes(8);
  CHECK(bitmap->FindStream({})->ReadData(0, bitmap_bytes));

  CHECK_THAT(Tests::TakeCapturedLog(), !ContainsSubstring(bad_message));
}

}  // namespace

TEST_CASE("a live file on a bad cluster is logged in recover mode",
          "[alloc][bad-live-check]") {
  CheckABadClusterIsLogged<Cache::Strategy::NoCache>();
  CheckABadClusterIsLogged<Cache::Strategy::FullCache>();
}

TEST_CASE("a live run that is only partly bad is logged",
          "[alloc][bad-live-check]") {
  CheckAPartlyBadRunIsLogged<Cache::Strategy::NoCache>();
  CheckAPartlyBadRunIsLogged<Cache::Strategy::FullCache>();
}

TEST_CASE("a healthy live file raises neither diagnostic",
          "[alloc][bad-live-check]") {
  CheckAHealthyFileIsNotLogged<Cache::Strategy::NoCache>();
  CheckAHealthyFileIsNotLogged<Cache::Strategy::FullCache>();
}

TEST_CASE("strict mode reads no bad list when it reads a live file",
          "[alloc][bad-live-check]") {
  CheckStrictModeReadsNoBadList<Cache::Strategy::NoCache>();
  CheckStrictModeReadsNoBadList<Cache::Strategy::FullCache>();
}

TEST_CASE("a cluster listed bad and clear in the bitmap raises one message",
          "[alloc][bad-live-check]") {
  CheckOneCauseRaisesOneMessage<Cache::Strategy::NoCache>();
  CheckOneCauseRaisesOneMessage<Cache::Strategy::FullCache>();
}

TEST_CASE("the bad cluster check runs once per stream",
          "[alloc][bad-live-check]") {
  CheckTheCheckRunsOnce<Cache::Strategy::NoCache>();
  CheckTheCheckRunsOnce<Cache::Strategy::FullCache>();
}

TEST_CASE("a deleted file is not checked for bad clusters",
          "[alloc][bad-live-check]") {
  CheckADeletedFileIsNotChecked<Cache::Strategy::NoCache>();
  CheckADeletedFileIsNotChecked<Cache::Strategy::FullCache>();
}

TEST_CASE("the volume's own metadata is not checked for bad clusters",
          "[alloc][bad-live-check]") {
  CheckTheVolumesOwnMetadataIsNotChecked<Cache::Strategy::NoCache>();
  CheckTheVolumesOwnMetadataIsNotChecked<Cache::Strategy::FullCache>();
}
