#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "counting-disk-reader.h"
#include "io/file-reader.h"
#include "memory-disk-reader.h"
#include "recording-disk-reader.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using Kind = Tests::CountingDiskReader::Kind;

namespace {

// Size of the buffer the wrapped reader holds: four cache blocks.
constexpr size_t buffer_bytes = size_t{256} * 1024;

// A hint inside the second cache block of 64 KiB, and one across the end of the
// first block.
constexpr ULONGLONG inner_hint_offset = 70000;
constexpr ULONGLONG inner_hint_length = 1000;
constexpr ULONGLONG straddling_hint_offset = 65000;

// The cache block of FullCache.
constexpr ULONGLONG block_bytes = ULONGLONG{64} * 1024;

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
  CHECK(IsHint(calls.at(0), block_bytes, block_bytes));
  CHECK(IsHint(calls.at(1), 0, 2 * block_bytes));
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
