#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "alloc/cluster-bitmap.h"
#include "fake-ntfs-image.h"
#include "mft/tree-testing.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::DataState;
using Tests::FakeDataRun;

namespace {

// Freed files in the image, and clusters of each one's run.
constexpr size_t file_count = 8;
constexpr DWORD run_clusters = 4;

// Bytes of bitmap in a window: 32 clusters, so the 128-cluster volume has four.
constexpr size_t small_window = 4;

// Restores the defaults when a test ends, however it ends.
class Overrides {
 public:
  Overrides(size_t window_bytes, size_t pending_cap) {
    Alloc::SetBitmapWindowBytesForTests(window_bytes);
    Mft::SetMaxPendingRangesForTests(pending_cap);
  }

  Overrides(const Overrides&) = delete;
  Overrides& operator=(const Overrides&) = delete;

  ~Overrides() {
    Alloc::SetBitmapWindowBytesForTests(0);
    Mft::SetMaxPendingRangesForTests(0);
  }
};

// The image, which of its files have a reused cluster, and where the bitmap
// data begins.
struct Fixture {
  Tests::FakeManyDeleted built;
  std::vector<bool> reused;
  std::vector<BYTE> image;
  ULONGLONG data_offset{0};
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.built =
      Tests::BuildFakeNtfsImageWithManyDeletedFiles(file_count, run_clusters);
  std::vector<FakeDataRun> allocated;
  for (size_t i = 0; i < file_count; i++) {
    const bool reused = (i % 3) == 1;
    fixture.reused.push_back(reused);
    if (reused) {
      // The last cluster of the run, so the whole run has to be judged.
      allocated.push_back(
          {.lcn = fixture.built.lcns.at(i) + run_clusters - 1, .clusters = 1});
    }
  }
  fixture.image = fixture.built.image;
  const std::vector<BYTE> bits =
      Tests::AllocatedBitmapFor(fixture.image, allocated);
  fixture.data_offset = fixture.image.size();
  Tests::PutClusterBitmap(fixture.image, bits);
  return fixture;
}

// What a scan reports for each freed file, and the reads it made of the bitmap.
struct Scanned {
  std::vector<std::optional<DataState>> states;
  size_t bitmap_reads{0};
};

Scanned Scan(const Fixture& fixture) {
  const auto opened = Support::Open<Cache::Strategy::NoCache>(fixture.image);
  const Mft::MftTree tree(*opened.volume);

  Scanned scanned;
  for (size_t i = 0; i < file_count; i++) {
    const Mft::MftTree::Entry* entry =
        tree.Find(Tests::many_deleted_first_idx + i);
    REQUIRE(entry != nullptr);
    scanned.states.push_back(entry->data_state);
  }
  scanned.bitmap_reads =
      opened.counter->ReadsInRange(fixture.data_offset, fixture.image.size());
  return scanned;
}

}  // namespace

TEST_CASE("a scan resolves scattered deleted files in sorted batches",
          "[mft][data-state][batch]") {
  const Fixture fixture = MakeFixture();
  const Overrides overrides(small_window, 0);

  const Scanned once = Scan(fixture);

  REQUIRE(once.states.size() == file_count);
  for (size_t i = 0; i < file_count; i++) {
    CHECK(once.states.at(i) ==
          (fixture.reused.at(i) ? DataState::Reallocated : DataState::Intact));
  }
  // The volume has four windows, so one pass over them reads each once.
  CHECK(once.bitmap_reads > 0);
  CHECK(once.bitmap_reads <= 4);
}

TEST_CASE("a small batch cap gives the same states with more flushes",
          "[mft][data-state][batch]") {
  const Fixture fixture = MakeFixture();

  Scanned whole;
  {
    const Overrides overrides(small_window, 0);
    whole = Scan(fixture);
  }
  Scanned flushed;
  {
    const Overrides overrides(small_window, 2);
    flushed = Scan(fixture);
  }
  Scanned one_by_one;
  {
    const Overrides overrides(small_window, 1);
    one_by_one = Scan(fixture);
  }

  CHECK(flushed.states == whole.states);
  CHECK(one_by_one.states == whole.states);
  // Every window has to be read once, which one sorted batch does at best.
  CHECK(whole.bitmap_reads <= flushed.bitmap_reads);
  CHECK(whole.bitmap_reads <= one_by_one.bitmap_reads);
}

TEST_CASE("annotation bitmap reads", "[.benchmark]") {
  // 10 000 deleted files scattered over a volume whose bitmap spans many
  // windows. A window of 128 bytes covers 1024 clusters, so a per-file query
  // would reload one for nearly every file.
  constexpr size_t many_files = 10000;
  constexpr size_t benchmark_window = 128;
  Tests::FakeManyDeleted built =
      Tests::BuildFakeNtfsImageWithManyDeletedFiles(many_files, run_clusters);
  std::vector<BYTE> image = std::move(built.image);
  const std::vector<BYTE> bits = Tests::AllocatedBitmapFor(image, {});
  const ULONGLONG data_offset = image.size();
  Tests::PutClusterBitmap(image, bits);

  const auto count_reads = [&](size_t pending_cap) {
    const Overrides overrides(benchmark_window, pending_cap);
    const auto opened = Support::Open<Cache::Strategy::NoCache>(image);
    const Mft::MftTree tree(*opened.volume);
    return opened.counter->ReadsInRange(data_offset, image.size());
  };

  const size_t batched = count_reads(0);
  const size_t one_by_one = count_reads(1);
  WARN("bitmap reads for " << many_files
                           << " deleted files: sorted batch = " << batched
                           << ", one query per file = " << one_by_one);
  CHECK(batched <= one_by_one);
}
