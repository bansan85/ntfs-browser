#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"
#include "ntfsundel-core.h"

namespace {

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using NtfsUndel::RecoverError;
using NtfsUndel::RecoveryAdvice;
using NtfsUndel::RecoveryCause;
using NtfsUndel::RecoveryCheck;
using Tests::FakeDataRun;

// The four freed files of the volume, one per data state.
constexpr std::uint64_t intact_ref = 21;
constexpr std::uint64_t reallocated_ref = 22;
constexpr std::uint64_t bad_ref = 23;
constexpr std::uint64_t unknown_ref = 24;

// Freed directory of BuildFakeNtfsImageWithFreedFileTree(). It has no $DATA.
constexpr std::uint64_t freed_dir_ref = 20;

// Bytes of each file: two clusters.
constexpr std::uint64_t file_size = 2 * Tests::fake_cluster_size;

// Where each file's two clusters are. The unknown file's run lies far past the
// volume.
constexpr ULONGLONG intact_lcn = 30;
constexpr ULONGLONG reallocated_lcn = 33;
constexpr ULONGLONG bad_lcn = 36;
constexpr ULONGLONG past_the_volume_lcn = 5000;

// The bad cluster of the third file, marked allocated in the bitmap as NTFS
// does.
const std::vector<Alloc::Extent> bad_cluster{
    {.lcn = bad_lcn + 1, .clusters = 1}};

// The freed tree with the four files written over its records 21 to 24, a
// bitmap that gives one cluster of the second file to another file, and a bad
// list. Each file's clusters hold a pattern, so a copy can be compared.
std::vector<BYTE> FourStatesImage() {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  const auto put = [&image](std::uint64_t idx, ULONGLONG lcn,
                            ULONGLONG real_size = file_size) {
    Tests::PutFreedNonResidentRecord(
        image, idx,
        {.real_size = real_size, .runs = {{.lcn = lcn, .clusters = 2}}});
  };
  put(intact_ref, intact_lcn);
  put(reallocated_ref, reallocated_lcn);
  put(bad_ref, bad_lcn);
  put(unknown_ref, past_the_volume_lcn);
  for (const ULONGLONG lcn : {intact_lcn, reallocated_lcn, bad_lcn}) {
    for (size_t i = 0; i < file_size; i++) {
      image.at(lcn * Tests::fake_cluster_size + i) =
          static_cast<BYTE>(lcn + i * 3);
    }
  }

  const std::vector<FakeDataRun> other_files{
      FakeDataRun{.lcn = reallocated_lcn + 1, .clusters = 1}};
  image = Support::WithBitmap(std::move(image),
                              Support::WithBadRuns(other_files, bad_cluster));
  return Support::WithBadClusters(std::move(image), bad_cluster);
}

using Volume = NTFS_BROWSER_NAMESPACE::NtfsVolume<Cache::Strategy::NoCache>;

// A freed record, parsed with its data attributes.
struct Opened {
  Support::Opened<Cache::Strategy::NoCache> volume;
  NtfsUndel::OpenedRecord record;
};

Opened Open(const std::vector<BYTE>& image, std::uint64_t reference) {
  Opened opened{.volume = Support::OpenVolume<Cache::Strategy::NoCache>(image),
                .record = {}};
  opened.record = NtfsUndel::OpenDeletedFile(*opened.volume.volume, reference);
  REQUIRE(opened.record.error == RecoverError::None);
  REQUIRE(opened.record.record != nullptr);
  return opened;
}

// Whether message holds text.
bool Mentions(const std::wstring& message, std::wstring_view text) {
  return message.find(text) != std::wstring::npos;
}

// What a scripted confirm saw.
struct Asked {
  int calls = 0;
  RecoveryCheck last;
};

// What a copy did: its result and the bytes written.
struct Copied {
  NtfsUndel::CopyResult result;
  std::vector<BYTE> bytes;
  int large_calls = 0;
};

Copied Copy(const NtfsUndel::OpenedRecord& opened, bool accept_large = true) {
  Copied copied;
  copied.result = NtfsUndel::CopyUnnamedData(
      *opened.record,
      [&copied, accept_large](std::uint64_t) {
        ++copied.large_calls;
        return accept_large;
      },
      [&copied](std::span<const BYTE> chunk) {
        copied.bytes.insert(copied.bytes.end(), chunk.begin(), chunk.end());
        return true;
      });
  return copied;
}

}  // namespace

TEST_CASE("an intact freed file is recovered with no question",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), intact_ref);

  const RecoveryCheck check = NtfsUndel::AssessRecovery(*opened.record.record);
  Asked asked;
  const bool go_on = NtfsUndel::ShouldRecover(*opened.record.record,
                                              [&asked](const RecoveryCheck&) {
                                                ++asked.calls;
                                                return false;
                                              });

  CHECK(check.advice == RecoveryAdvice::Proceed);
  CHECK(check.cause == RecoveryCause::None);
  CHECK(go_on);
  CHECK(asked.calls == 0);
  CHECK(NtfsUndel::RecoveryMessage(check).empty());
}

TEST_CASE("a reallocated freed file asks first, and a refusal stops the copy",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), reallocated_ref);

  const RecoveryCheck check = NtfsUndel::AssessRecovery(*opened.record.record);
  Asked asked;
  const bool refused = NtfsUndel::ShouldRecover(
      *opened.record.record, [&asked](const RecoveryCheck& seen) {
        ++asked.calls;
        asked.last = seen;
        return false;
      });
  const bool accepted = NtfsUndel::ShouldRecover(
      *opened.record.record, [](const RecoveryCheck&) { return true; });

  CHECK(check.advice == RecoveryAdvice::Confirm);
  CHECK(check.cause == RecoveryCause::Reallocated);
  CHECK_FALSE(refused);
  CHECK(asked.calls == 1);
  CHECK(asked.last.cause == RecoveryCause::Reallocated);
  CHECK(accepted);
  CHECK(Mentions(NtfsUndel::RecoveryMessage(check), L"another file"));
}

TEST_CASE("a freed file on a bad cluster asks first, naming the bad clusters",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), bad_ref);

  const RecoveryCheck check = NtfsUndel::AssessRecovery(*opened.record.record);
  Asked asked;
  const bool refused = NtfsUndel::ShouldRecover(
      *opened.record.record, [&asked](const RecoveryCheck& seen) {
        ++asked.calls;
        asked.last = seen;
        return false;
      });

  CHECK(check.advice == RecoveryAdvice::Confirm);
  CHECK(check.cause == RecoveryCause::BadClusters);
  CHECK_FALSE(refused);
  CHECK(asked.calls == 1);
  CHECK(asked.last.cause == RecoveryCause::BadClusters);
  CHECK(Mentions(NtfsUndel::RecoveryMessage(check), L"marked bad"));
}

TEST_CASE("a freed file whose state is unknown warns and the copy runs",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), unknown_ref);

  const RecoveryCheck check = NtfsUndel::AssessRecovery(*opened.record.record);
  Asked asked;
  const bool go_on = NtfsUndel::ShouldRecover(
      *opened.record.record, [&asked](const RecoveryCheck& seen) {
        ++asked.calls;
        asked.last = seen;
        return true;
      });

  CHECK(check.advice == RecoveryAdvice::Warn);
  CHECK(check.cause == RecoveryCause::Unchecked);
  CHECK(go_on);
  CHECK(asked.calls == 1);
  CHECK(Mentions(NtfsUndel::RecoveryMessage(check), L"could not be checked"));
}

TEST_CASE("a freed record with no unnamed stream goes ahead",
          "[undel][advice]") {
  const std::vector<BYTE> image = FourStatesImage();
  Opened opened{.volume = Support::OpenVolume<Cache::Strategy::NoCache>(image),
                .record = {}};
  opened.record =
      NtfsUndel::OpenDeletedFile(*opened.volume.volume, freed_dir_ref);
  REQUIRE(opened.record.error == RecoverError::None);

  const RecoveryCheck check = NtfsUndel::AssessRecovery(*opened.record.record);

  CHECK(check.advice == RecoveryAdvice::Proceed);
}

TEST_CASE("after a yes the copy reads exactly what the disk holds",
          "[undel][advice]") {
  const std::vector<BYTE> image = FourStatesImage();

  SECTION("a file on a bad cluster is copied whole, bad cluster included") {
    const Opened opened = Open(image, bad_ref);
    REQUIRE(NtfsUndel::ShouldRecover(
        *opened.record.record, [](const RecoveryCheck&) { return true; }));

    const Copied copied = Copy(opened.record);

    CHECK(copied.result.error == RecoverError::None);
    CHECK(copied.result.bytes == file_size);
    const std::vector<BYTE> expected(
        image.begin() + bad_lcn * Tests::fake_cluster_size,
        image.begin() + bad_lcn * Tests::fake_cluster_size + file_size);
    CHECK(copied.bytes == expected);
  }

  SECTION("a read the disk cannot serve ends the copy with the read error") {
    const Opened opened = Open(image, unknown_ref);

    const Copied copied = Copy(opened.record);

    CHECK(copied.result.error == RecoverError::Read);
  }
}

TEST_CASE("a large file that is not intact is asked about twice, once each",
          "[undel][advice]") {
  std::vector<BYTE> image = FourStatesImage();
  // The bad file again, now declaring 150 MiB: over the large-file limit.
  Tests::PutFreedNonResidentRecord(image, bad_ref,
                                   {.real_size = 150ULL * 1024 * 1024,
                                    .runs = {{.lcn = bad_lcn, .clusters = 2}}});
  const Opened opened = Open(image, bad_ref);

  Asked asked;
  const bool go_on = NtfsUndel::ShouldRecover(*opened.record.record,
                                              [&asked](const RecoveryCheck&) {
                                                ++asked.calls;
                                                return true;
                                              });
  const Copied copied = Copy(opened.record, false);

  CHECK(go_on);
  CHECK(asked.calls == 1);
  CHECK(copied.large_calls == 1);
  CHECK(copied.result.error == RecoverError::Declined);
}

TEST_CASE("the recovery messages name their cause and are empty for none",
          "[undel][advice]") {
  CHECK(NtfsUndel::RecoveryMessage({}).empty());
  CHECK_FALSE(NtfsUndel::RecoveryMessage({.advice = RecoveryAdvice::Confirm,
                                          .cause = RecoveryCause::Reallocated})
                  .empty());
  CHECK_FALSE(NtfsUndel::RecoveryMessage({.advice = RecoveryAdvice::Confirm,
                                          .cause = RecoveryCause::BadClusters})
                  .empty());
  CHECK_FALSE(NtfsUndel::RecoveryMessage({.advice = RecoveryAdvice::Warn,
                                          .cause = RecoveryCause::Unchecked})
                  .empty());
}
