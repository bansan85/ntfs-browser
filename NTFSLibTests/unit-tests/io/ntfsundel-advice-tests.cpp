#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsundel/ntfsundel-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>

#include "alloc/types.h"
#include "win-types.h"

namespace ExampleNtfsUndel = NTFS_BROWSER_NAMESPACE::Example::NtfsUndel;

namespace {

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
using ExampleNtfsUndel::RecoverError;
using ExampleNtfsUndel::RecoveryAdvice;
using ExampleNtfsUndel::RecoveryCause;
using ExampleNtfsUndel::RecoveryCheck;
using NTFS_BROWSER_NAMESPACE::Test::Io::TempImage;
using TestFakeImage::FakeDataRun;

// The four freed files of the volume, one per data state.
constexpr std::uint64_t intact_ref = 21;
constexpr std::uint64_t reallocated_ref = 22;
constexpr std::uint64_t bad_ref = 23;
constexpr std::uint64_t unknown_ref = 24;

// Freed directory of BuildFakeNtfsImageWithFreedFileTree(). It has no $DATA.
constexpr std::uint64_t freed_dir_ref = 20;

// Bytes of each file: two clusters.
constexpr std::uint64_t file_size = 2 * TestFakeImage::fake_cluster_size;

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
  std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithFreedFileTree();
  const auto put = [&image](std::uint64_t idx, ULONGLONG lcn,
                            ULONGLONG real_size = file_size) {
    TestFakeImage::PutFreedNonResidentRecord(
        image, idx,
        {.real_size = real_size, .runs = {{.lcn = lcn, .clusters = 2}}});
  };
  put(intact_ref, intact_lcn);
  put(reallocated_ref, reallocated_lcn);
  put(bad_ref, bad_lcn);
  put(unknown_ref, past_the_volume_lcn);
  for (const ULONGLONG lcn : {intact_lcn, reallocated_lcn, bad_lcn}) {
    for (size_t i = 0; i < file_size; i++) {
      image.at(lcn * TestFakeImage::fake_cluster_size + i) =
          static_cast<BYTE>(lcn + i * 3);
    }
  }

  const std::vector<FakeDataRun> other_files{
      FakeDataRun{.lcn = reallocated_lcn + 1, .clusters = 1}};
  image = TestAlloc::WithBitmap(
      std::move(image), TestAlloc::WithBadRuns(other_files, bad_cluster));
  return TestAlloc::WithBadClusters(std::move(image), bad_cluster);
}

// A volume over a temp image, a Reader of it, and the handle of one record.
// The handle takes any sequence number: the tests name the records by number.
struct Opened {
  std::unique_ptr<TempImage> image;
  std::unique_ptr<Lib::Reader> reader;
  Lib::Handle handle;
};

Opened Open(const std::vector<BYTE>& image, std::uint64_t reference) {
  Opened opened;
  opened.image = std::make_unique<TempImage>(image);
  opened.reader = std::make_unique<Lib::Reader>(Lib::ReaderOptions{
      .target = ExampleNtfsUndel::ToUtf16(opened.image->path.wstring())});
  REQUIRE(opened.reader->IsOpen());
  opened.handle = {.record = reference};
  REQUIRE(opened.reader->Check(opened.handle));
  return opened;
}

// What the Reader says of the clusters of the opened file, as advice.
RecoveryCheck Assess(const Opened& opened) {
  return ExampleNtfsUndel::AssessRecovery(*opened.reader, opened.handle);
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
  ExampleNtfsUndel::CopyResult result;
  std::vector<BYTE> bytes;
  int large_calls = 0;
};

Copied Copy(const Opened& opened, bool accept_large = true) {
  Copied copied;
  copied.result = ExampleNtfsUndel::CopyUnnamedData(
      *opened.reader, opened.handle,
      [&copied, accept_large](std::uint64_t) {
        ++copied.large_calls;
        return accept_large;
      },
      [&copied](std::span<const std::uint8_t> chunk) {
        copied.bytes.insert(copied.bytes.end(), chunk.begin(), chunk.end());
        return true;
      });
  return copied;
}

}  // namespace

TEST_CASE("an intact freed file is recovered with no question",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), intact_ref);

  const RecoveryCheck check = Assess(opened);
  Asked asked;
  const bool go_on =
      ExampleNtfsUndel::ShouldRecover(check, [&asked](const RecoveryCheck&) {
        ++asked.calls;
        return false;
      });

  CHECK(check.advice == RecoveryAdvice::Proceed);
  CHECK(check.cause == RecoveryCause::None);
  CHECK(go_on);
  CHECK(asked.calls == 0);
  CHECK(ExampleNtfsUndel::RecoveryMessage(check).empty());
}

TEST_CASE("a reallocated freed file asks first, and a refusal stops the copy",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), reallocated_ref);

  const RecoveryCheck check = Assess(opened);
  Asked asked;
  const bool refused = ExampleNtfsUndel::ShouldRecover(
      check, [&asked](const RecoveryCheck& seen) {
        ++asked.calls;
        asked.last = seen;
        return false;
      });
  const bool accepted = ExampleNtfsUndel::ShouldRecover(
      check, [](const RecoveryCheck&) { return true; });

  CHECK(check.advice == RecoveryAdvice::Confirm);
  CHECK(check.cause == RecoveryCause::Reallocated);
  CHECK_FALSE(refused);
  CHECK(asked.calls == 1);
  CHECK(asked.last.cause == RecoveryCause::Reallocated);
  CHECK(accepted);
  CHECK(Mentions(ExampleNtfsUndel::RecoveryMessage(check), L"another file"));
}

TEST_CASE("a freed file on a bad cluster asks first, naming the bad clusters",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), bad_ref);

  const RecoveryCheck check = Assess(opened);
  Asked asked;
  const bool refused = ExampleNtfsUndel::ShouldRecover(
      check, [&asked](const RecoveryCheck& seen) {
        ++asked.calls;
        asked.last = seen;
        return false;
      });

  CHECK(check.advice == RecoveryAdvice::Confirm);
  CHECK(check.cause == RecoveryCause::BadClusters);
  CHECK_FALSE(refused);
  CHECK(asked.calls == 1);
  CHECK(asked.last.cause == RecoveryCause::BadClusters);
  CHECK(Mentions(ExampleNtfsUndel::RecoveryMessage(check), L"marked bad"));
}

TEST_CASE("a freed file whose state is unknown warns and the copy runs",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), unknown_ref);

  const RecoveryCheck check = Assess(opened);
  Asked asked;
  const bool go_on = ExampleNtfsUndel::ShouldRecover(
      check, [&asked](const RecoveryCheck& seen) {
        ++asked.calls;
        asked.last = seen;
        return true;
      });

  CHECK(check.advice == RecoveryAdvice::Warn);
  CHECK(check.cause == RecoveryCause::Unchecked);
  CHECK(go_on);
  CHECK(asked.calls == 1);
  CHECK(Mentions(ExampleNtfsUndel::RecoveryMessage(check),
                 L"could not be checked"));
}

TEST_CASE("a freed record with no unnamed stream goes ahead",
          "[undel][advice]") {
  const Opened opened = Open(FourStatesImage(), freed_dir_ref);

  const RecoveryCheck check = Assess(opened);

  CHECK(check.advice == RecoveryAdvice::Proceed);
}

TEST_CASE("the advice follows the data state", "[undel][advice]") {
  using Lib::DataState;
  CHECK(ExampleNtfsUndel::AssessRecovery(DataState::NotChecked).advice ==
        RecoveryAdvice::Proceed);
  CHECK(ExampleNtfsUndel::AssessRecovery(DataState::Intact).advice ==
        RecoveryAdvice::Proceed);
  CHECK(ExampleNtfsUndel::AssessRecovery(DataState::Reallocated).cause ==
        RecoveryCause::Reallocated);
  CHECK(ExampleNtfsUndel::AssessRecovery(DataState::Bad).cause ==
        RecoveryCause::BadClusters);
  CHECK(ExampleNtfsUndel::AssessRecovery(DataState::Unknown).advice ==
        RecoveryAdvice::Warn);
}

TEST_CASE("after a yes the copy reads exactly what the disk holds",
          "[undel][advice]") {
  const std::vector<BYTE> image = FourStatesImage();

  SECTION("a file on a bad cluster is copied whole, bad cluster included") {
    const Opened opened = Open(image, bad_ref);
    REQUIRE(ExampleNtfsUndel::ShouldRecover(
        Assess(opened), [](const RecoveryCheck&) { return true; }));

    const Copied copied = Copy(opened);

    CHECK(copied.result.error == RecoverError::None);
    CHECK(copied.result.bytes == file_size);
    const std::vector<BYTE> expected(
        image.begin() + bad_lcn * TestFakeImage::fake_cluster_size,
        image.begin() + bad_lcn * TestFakeImage::fake_cluster_size + file_size);
    CHECK(copied.bytes == expected);
  }

  SECTION("a read the disk cannot serve ends the copy with the read error") {
    const Opened opened = Open(image, unknown_ref);

    const Copied copied = Copy(opened);

    CHECK(copied.result.error == RecoverError::Read);
  }
}

TEST_CASE("a large file that is not intact is asked about twice, once each",
          "[undel][advice]") {
  std::vector<BYTE> image = FourStatesImage();
  // The bad file again, now declaring 150 MiB: over the large-file limit.
  TestFakeImage::PutFreedNonResidentRecord(
      image, bad_ref,
      {.real_size = 150ULL * 1024 * 1024,
       .runs = {{.lcn = bad_lcn, .clusters = 2}}});
  const Opened opened = Open(image, bad_ref);

  Asked asked;
  const bool go_on = ExampleNtfsUndel::ShouldRecover(
      Assess(opened), [&asked](const RecoveryCheck&) {
        ++asked.calls;
        return true;
      });
  const Copied copied = Copy(opened, false);

  CHECK(go_on);
  CHECK(asked.calls == 1);
  CHECK(copied.large_calls == 1);
  CHECK(copied.result.error == RecoverError::Declined);
}

TEST_CASE("the recovery messages name their cause and are empty for none",
          "[undel][advice]") {
  CHECK(ExampleNtfsUndel::RecoveryMessage({}).empty());
  CHECK_FALSE(
      ExampleNtfsUndel::RecoveryMessage({.advice = RecoveryAdvice::Confirm,
                                         .cause = RecoveryCause::Reallocated})
          .empty());
  CHECK_FALSE(
      ExampleNtfsUndel::RecoveryMessage({.advice = RecoveryAdvice::Confirm,
                                         .cause = RecoveryCause::BadClusters})
          .empty());
  CHECK_FALSE(
      ExampleNtfsUndel::RecoveryMessage(
          {.advice = RecoveryAdvice::Warn, .cause = RecoveryCause::Unchecked})
          .empty());
}
