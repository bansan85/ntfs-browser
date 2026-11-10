#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>

#include "io/guarded-disk-reader.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader;

namespace {

// A short buffer whose bytes all differ, so that a misplaced copy shows.
std::vector<BYTE> Content() { return {10, 11, 12, 13, 14, 15, 16, 17}; }

}  // namespace

TEST_CASE("GuardedDiskReader reads through the wrapped reader",
          "[io][guarded]") {
  Io::GuardedDiskReader guarded(std::make_unique<MemoryDiskReader>(Content()));
  std::array<BYTE, 3> bytes{};

  REQUIRE(guarded.Read(2, bytes));
  CHECK(bytes == std::array<BYTE, 3>{12, 13, 14});
  CHECK(guarded.Size() == Content().size());
}

TEST_CASE("GuardedDiskReader fails a read past the end of the reader",
          "[io][guarded]") {
  Io::GuardedDiskReader guarded(std::make_unique<MemoryDiskReader>(Content()));
  std::array<BYTE, 4> bytes{};

  CHECK_FALSE(guarded.Read(6, bytes));
}

TEST_CASE("GuardedDiskReader without a reader answers and fails quietly",
          "[io][guarded]") {
  Io::GuardedDiskReader guarded(nullptr);
  std::array<BYTE, 2> bytes{};
  const std::atomic<bool> cancelled{false};

  CHECK_FALSE(guarded.Open(L"anything"));
  CHECK_FALSE(guarded.Read(0, bytes));
  CHECK_FALSE(guarded.ReadUnlessCancelled(cancelled, 0, bytes));
  CHECK_FALSE(guarded.Size().has_value());
  CHECK(guarded.GetMediaKind() == Tuning::MediaKind::Unknown);
  guarded.Prefetch(0, 1);
  guarded.CancelPrefetch();
  guarded.PrefetchStanding(0, 1);
  guarded.CancelStanding(0, 1);
  guarded.SetReadPattern(Tuning::Pattern::DependentSmall);
}

TEST_CASE("GuardedDiskReader skips the read of a cancelled chunk",
          "[io][guarded]") {
  Io::GuardedDiskReader guarded(std::make_unique<MemoryDiskReader>(Content()));
  std::array<BYTE, 2> bytes{};
  std::atomic<bool> cancelled{false};

  REQUIRE(guarded.ReadUnlessCancelled(cancelled, 4, bytes));
  CHECK(bytes == std::array<BYTE, 2>{14, 15});

  cancelled = true;
  bytes = {};
  CHECK_FALSE(guarded.ReadUnlessCancelled(cancelled, 4, bytes));
  CHECK(bytes == std::array<BYTE, 2>{0, 0});
}
