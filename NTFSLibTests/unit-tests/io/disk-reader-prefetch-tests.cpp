#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/disk-readers/recording-disk-reader.h>
#include <ntfs-browser/test/disk-readers/throttled-disk-reader.h>
#include <ntfs-browser/tuning/tuning.h>

#include "io/file-reader.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using Kind = Tests::CountingDiskReader::Kind;

namespace {

// Size of the buffer the wrapped reader holds: four cache blocks.
constexpr size_t buffer_bytes = size_t{256} * 1024;

// A hint inside a cache block, and one across the end of a block.
constexpr ULONGLONG inner_hint_offset = 70000;
constexpr ULONGLONG inner_hint_length = 1000;
constexpr ULONGLONG straddling_hint_offset = 65000;

// The cache block of FullCache: the default of the tuning.
constexpr ULONGLONG block_bytes =
    NTFS_BROWSER_NAMESPACE::Tuning::Settings{}.cache.block_bytes;

// The whole blocks that hold [offset, offset + length).
std::pair<ULONGLONG, ULONGLONG> Widened(ULONGLONG offset, ULONGLONG length) {
  const ULONGLONG first = offset - (offset % block_bytes);
  const ULONGLONG end =
      ((offset + length + block_bytes - 1) / block_bytes) * block_bytes;
  return {first, end - first};
}

// A CountingDiskReader over a zeroed buffer, and a pointer to it.
struct Counted {
  std::unique_ptr<Tests::CountingDiskReader> reader;
  Tests::CountingDiskReader* counting;
};

Counted MakeCounted() {
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(
          std::vector<BYTE>(buffer_bytes)));
  Tests::CountingDiskReader* const counting = reader.get();
  return {std::move(reader), counting};
}

// Whether call is the prefetch hint [offset, offset + length).
bool IsHint(const Tests::CountingDiskReader::Call& call, ULONGLONG offset,
            ULONGLONG length) {
  return call.kind == Kind::Prefetch && call.offset == offset &&
         call.length == length;
}

// Whether call is the standing hint [offset, offset + length).
bool IsStandingHint(const Tests::CountingDiskReader::Call& call,
                    ULONGLONG offset, ULONGLONG length) {
  return call.kind == Kind::PrefetchStanding && call.offset == offset &&
         call.length == length;
}

// Whether call withdraws the standing interval [offset, offset + length).
bool IsStandingCancel(const Tests::CountingDiskReader::Call& call,
                      ULONGLONG offset, ULONGLONG length) {
  return call.kind == Kind::CancelStanding && call.offset == offset &&
         call.length == length;
}

// The largest offset: an interval that runs to it withdraws everything.
constexpr ULONGLONG max_offset = (std::numeric_limits<ULONGLONG>::max)();

// A path in the temp directory that nothing uses, removed with the object.
struct TempPath final {
  std::filesystem::path path;

  TempPath() {
    std::random_device random_device;
    path = std::filesystem::temp_directory_path() /
           ("ntfsbrowser-prefetch-test-" + std::to_string(random_device()));
  }

  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  TempPath(TempPath&&) = delete;
  TempPath& operator=(TempPath&&) = delete;

  ~TempPath() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

}  // namespace

TEST_CASE("a reader that overrides nothing ignores prefetch hints",
          "[disk-reader][prefetch]") {
  const std::vector<BYTE> content{1, 2, 3, 4};
  const Tests::MemoryDiskReader reader(content);
  reader.Prefetch(0, content.size());
  reader.CancelPrefetch();

  std::vector<BYTE> dest(content.size());
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, dest));
  CHECK(dest == content);
}

TEST_CASE("RecordingDiskReader passes prefetch hints on and records nothing",
          "[disk-reader][prefetch]") {
  const TempPath trace;
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Tests::RecordingDiskReader reader(std::move(counted.reader),
                                          trace.path);

  reader.Prefetch(inner_hint_offset, inner_hint_length);
  reader.CancelPrefetch();

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 2);
  CHECK(IsHint(calls.at(0), inner_hint_offset, inner_hint_length));
  CHECK(calls.at(1).kind == Kind::CancelPrefetch);
  CHECK(std::filesystem::file_size(trace.path) == 0);
}

TEST_CASE("ReadAheadDiskReader passes prefetch hints on to the wrapped reader",
          "[disk-reader][prefetch][read-ahead]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::ReadAheadDiskReader reader(std::move(counted.reader));

  reader.Prefetch(inner_hint_offset, inner_hint_length);
  reader.CancelPrefetch();

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 2);
  CHECK(IsHint(calls.at(0), inner_hint_offset, inner_hint_length));
  CHECK(calls.at(1).kind == Kind::CancelPrefetch);
}

TEST_CASE("ReadAheadDiskReader without a wrapped reader ignores prefetch hints",
          "[disk-reader][prefetch][read-ahead]") {
  const Io::ReadAheadDiskReader reader(nullptr);
  reader.Prefetch(inner_hint_offset, inner_hint_length);
  reader.CancelPrefetch();
}

TEST_CASE("FileReader<NoCache> passes a prefetch hint on as it is",
          "[disk-reader][prefetch][file-reader]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::FileReader<Cache::Strategy::NoCache> reader(
      std::move(counted.reader));

  reader.Prefetch(inner_hint_offset, inner_hint_length);
  reader.Prefetch(inner_hint_offset, 0);
  reader.CancelPrefetch();

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 2);
  CHECK(IsHint(calls.at(0), inner_hint_offset, inner_hint_length));
  CHECK(calls.at(1).kind == Kind::CancelPrefetch);
}

TEST_CASE("FileReader<FullCache> widens a prefetch hint to its cache blocks",
          "[disk-reader][prefetch][file-reader]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::FileReader<Cache::Strategy::FullCache> reader(
      std::move(counted.reader));

  reader.Prefetch(inner_hint_offset, inner_hint_length);
  reader.Prefetch(straddling_hint_offset, inner_hint_length);
  reader.Prefetch(inner_hint_offset, 0);

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 2);
  const auto inner = Widened(inner_hint_offset, inner_hint_length);
  const auto straddling = Widened(straddling_hint_offset, inner_hint_length);
  // The straddling hint spans two blocks.
  CHECK(straddling.second == 2 * block_bytes);
  CHECK(IsHint(calls.at(0), inner.first, inner.second));
  CHECK(IsHint(calls.at(1), straddling.first, straddling.second));
}

TEMPLATE_TEST_CASE_SIG("A FileReader with no reader ignores prefetch hints",
                       "[disk-reader][prefetch][file-reader]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const Io::FileReader<S> reader;
  reader.Prefetch(inner_hint_offset, inner_hint_length);
  reader.CancelPrefetch();
  CHECK_FALSE(reader.Size().has_value());
}

TEST_CASE("a reader that overrides nothing ignores standing hints",
          "[disk-reader][prefetch][standing]") {
  const std::vector<BYTE> content{1, 2, 3, 4};
  const Tests::MemoryDiskReader reader(content);
  reader.PrefetchStanding(0, content.size());
  reader.CancelStanding(0, content.size());
  reader.CancelStanding(0, max_offset);

  std::vector<BYTE> dest(content.size());
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, dest));
  CHECK(dest == content);
}

TEST_CASE("CountingDiskReader records the standing hints and passes them on",
          "[disk-reader][prefetch][standing]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;

  counting->PrefetchStanding(inner_hint_offset, inner_hint_length);
  counting->Prefetch(inner_hint_offset, inner_hint_length);
  counting->CancelStanding(inner_hint_offset, 1);
  counting->CancelPrefetch();

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 4);
  CHECK(IsStandingHint(calls.at(0), inner_hint_offset, inner_hint_length));
  CHECK(IsHint(calls.at(1), inner_hint_offset, inner_hint_length));
  CHECK(IsStandingCancel(calls.at(2), inner_hint_offset, 1));
  CHECK(calls.at(3).kind == Kind::CancelPrefetch);
  CHECK(counting->ReadCount() == 0);
}

TEST_CASE("RecordingDiskReader passes standing hints on and records nothing",
          "[disk-reader][prefetch][standing]") {
  const TempPath trace;
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Tests::RecordingDiskReader reader(std::move(counted.reader),
                                          trace.path);

  reader.PrefetchStanding(inner_hint_offset, inner_hint_length);
  reader.CancelStanding(inner_hint_offset, inner_hint_length);

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 2);
  CHECK(IsStandingHint(calls.at(0), inner_hint_offset, inner_hint_length));
  CHECK(IsStandingCancel(calls.at(1), inner_hint_offset, inner_hint_length));
  CHECK(std::filesystem::file_size(trace.path) == 0);
}

TEST_CASE("ReadAheadDiskReader passes standing hints on to the wrapped reader",
          "[disk-reader][prefetch][standing][read-ahead]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::ReadAheadDiskReader reader(std::move(counted.reader));

  reader.PrefetchStanding(inner_hint_offset, inner_hint_length);
  reader.CancelStanding(inner_hint_offset, inner_hint_length);

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 2);
  CHECK(IsStandingHint(calls.at(0), inner_hint_offset, inner_hint_length));
  CHECK(IsStandingCancel(calls.at(1), inner_hint_offset, inner_hint_length));
}

TEST_CASE("ReadAheadDiskReader without a wrapped reader ignores standing hints",
          "[disk-reader][prefetch][standing][read-ahead]") {
  const Io::ReadAheadDiskReader reader(nullptr);
  reader.PrefetchStanding(inner_hint_offset, inner_hint_length);
  reader.CancelStanding(inner_hint_offset, inner_hint_length);
}

TEST_CASE("FileReader<NoCache> passes a standing hint on as it is",
          "[disk-reader][prefetch][standing][file-reader]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::FileReader<Cache::Strategy::NoCache> reader(
      std::move(counted.reader));

  reader.PrefetchStanding(inner_hint_offset, inner_hint_length);
  reader.PrefetchStanding(inner_hint_offset, 0);
  reader.CancelStanding(inner_hint_offset, inner_hint_length);
  reader.CancelStanding(inner_hint_offset, 0);
  reader.CancelStanding(0, max_offset);

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 3);
  CHECK(IsStandingHint(calls.at(0), inner_hint_offset, inner_hint_length));
  CHECK(IsStandingCancel(calls.at(1), inner_hint_offset, inner_hint_length));
  CHECK(IsStandingCancel(calls.at(2), 0, max_offset));
}

TEST_CASE("FileReader<FullCache> widens a standing hint to its cache blocks",
          "[disk-reader][prefetch][standing][file-reader]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::FileReader<Cache::Strategy::FullCache> reader(
      std::move(counted.reader));

  reader.PrefetchStanding(inner_hint_offset, inner_hint_length);
  reader.PrefetchStanding(straddling_hint_offset, inner_hint_length);
  reader.PrefetchStanding(inner_hint_offset, 0);
  reader.CancelStanding(inner_hint_offset, 1);
  reader.CancelStanding(inner_hint_offset, 0);

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 3);
  const auto inner = Widened(inner_hint_offset, inner_hint_length);
  const auto straddling = Widened(straddling_hint_offset, inner_hint_length);
  CHECK(IsStandingHint(calls.at(0), inner.first, inner.second));
  CHECK(IsStandingHint(calls.at(1), straddling.first, straddling.second));
  // The interval to withdraw is widened the same way.
  const auto one_byte = Widened(inner_hint_offset, 1);
  CHECK(IsStandingCancel(calls.at(2), one_byte.first, one_byte.second));
}

TEST_CASE("FileReader<FullCache> withdraws up to the largest offset",
          "[disk-reader][prefetch][standing][file-reader]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::FileReader<Cache::Strategy::FullCache> reader(
      std::move(counted.reader));

  reader.CancelStanding(0, max_offset);
  reader.CancelStanding(inner_hint_offset, max_offset);
  reader.CancelStanding(max_offset - 1, 1);

  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 3);
  const ULONGLONG first = inner_hint_offset - (inner_hint_offset % block_bytes);
  // An interval that runs to the end is cut there, and so is one that starts
  // in the last block: the widening never wraps.
  CHECK(IsStandingCancel(calls.at(0), 0, max_offset));
  CHECK(IsStandingCancel(calls.at(1), first, max_offset - first));
  CHECK(calls.at(2).kind == Kind::CancelStanding);
  CHECK(calls.at(2).offset + calls.at(2).length == max_offset);
}

TEMPLATE_TEST_CASE_SIG("A FileReader with no reader ignores standing hints",
                       "[disk-reader][prefetch][standing][file-reader]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const Io::FileReader<S> reader;
  reader.PrefetchStanding(inner_hint_offset, inner_hint_length);
  reader.CancelStanding(inner_hint_offset, inner_hint_length);
  CHECK_FALSE(reader.Size().has_value());
}

TEST_CASE("FileReader<FullCache> serves a run of missing blocks from the lane",
          "[disk-reader][prefetch][standing][file-reader]") {
  Tests::SimulatedClock clock;
  auto throttled = std::make_unique<Tests::ThrottledDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(
          std::vector<BYTE>(buffer_bytes)),
      Tests::DiskModel{}, clock);
  const Tests::ThrottledDiskReader* const disk = throttled.get();
  const Io::FileReader<Cache::Strategy::FullCache> reader(std::move(throttled));

  // The range starts and ends inside a block: the hint widens to three blocks,
  // and the read that follows makes one disk read of that run.
  const ULONGLONG offset = 5 * block_bytes + 100;
  const ULONGLONG length = 2 * block_bytes;
  reader.PrefetchStanding(offset, length);
  REQUIRE(disk->Stats().disk_reads == 1);

  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(offset)};
  const auto view = reader.Read(addr, static_cast<DWORD>(length));

  REQUIRE(view.has_value());
  CHECK(disk->Stats().served_reads == 1);
  CHECK(disk->Stats().unannounced_reads == 0);
  CHECK(disk->Stats().disk_reads == 1);
}

TEST_CASE("FileReader<FullCache> leaves the cached blocks out of the lane",
          "[disk-reader][prefetch][standing][file-reader]") {
  Counted counted = MakeCounted();
  Tests::CountingDiskReader* const counting = counted.counting;
  const Io::FileReader<Cache::Strategy::FullCache> reader(
      std::move(counted.reader));
  // The middle block of three is in the cache.
  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(6 * block_bytes)};
  const auto held = reader.Read(addr, static_cast<DWORD>(block_bytes));
  REQUIRE(held.has_value());
  counting->Reset();

  reader.PrefetchStanding(5 * block_bytes, 3 * block_bytes);
  reader.Prefetch(5 * block_bytes, 3 * block_bytes);

  // The same two runs, on both lanes.
  const auto& calls = counting->Calls();
  REQUIRE(calls.size() == 4);
  CHECK(IsStandingHint(calls.at(0), 5 * block_bytes, block_bytes));
  CHECK(IsStandingHint(calls.at(1), 7 * block_bytes, block_bytes));
  CHECK(IsHint(calls.at(2), 5 * block_bytes, block_bytes));
  CHECK(IsHint(calls.at(3), 7 * block_bytes, block_bytes));
}

TEMPLATE_TEST_CASE_SIG(
    "Standing hints change neither the bytes nor the reads of a reader that "
    "ignores them",
    "[disk-reader][prefetch][standing][file-reader]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // Reads at a few offsets, with the hints before and between them or not.
  const auto run = [](bool hinted) {
    std::vector<BYTE> image(buffer_bytes);
    for (size_t index = 0; index < image.size(); index++) {
      image.at(index) = static_cast<BYTE>((index * 11) + (index >> 8U));
    }
    auto counting = std::make_unique<Tests::CountingDiskReader>(
        std::make_unique<Tests::MemoryDiskReader>(image));
    Tests::CountingDiskReader* const counted = counting.get();
    const Io::FileReader<S> reader(std::move(counting));

    std::vector<BYTE> bytes;
    for (const ULONGLONG offset :
         {ULONGLONG{70000}, ULONGLONG{4096}, ULONGLONG{200000}}) {
      if (hinted) {
        reader.PrefetchStanding(offset, 3000);
        reader.PrefetchStanding(offset + 8192, 3000);
        reader.CancelStanding(offset + 8192, 1);
      }
      std::vector<BYTE> read(3000);
      LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(offset)};
      REQUIRE(reader.ReadInto(addr, read));
      bytes.insert(bytes.end(), read.begin(), read.end());
    }

    std::vector<std::pair<ULONGLONG, ULONGLONG>> reads;
    for (const Tests::CountingDiskReader::Call& call : counted->Calls()) {
      if (call.kind == Kind::Read) {
        reads.emplace_back(call.offset, call.length);
      }
    }
    return std::pair{bytes, reads};
  };

  const auto plain = run(false);
  const auto hinted = run(true);

  CHECK(hinted.first == plain.first);
  CHECK(hinted.second == plain.second);
}
