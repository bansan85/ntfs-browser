#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "io/file-reader.h"
#include "memory-disk-reader.h"
#include "partition-disk-reader.h"
#include "recording-disk-reader.h"
#include "sequential-disk-reader.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::IDiskReader;

namespace {

// Size of the buffer a MemoryDiskReader holds.
constexpr size_t buffer_bytes = 1234;

// Offset and length of a partition inside a larger disk image.
constexpr ULONGLONG partition_offset = 1048576;
constexpr ULONGLONG partition_length = 5242880;

// A reader that counts its calls and implements nothing else.
class CountingReader final : public IDiskReader {
 public:
  explicit CountingReader(std::optional<ULONGLONG> size) : size_(size) {}

  bool Open(std::wstring_view /*path*/) override { return true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& /*addr*/,
                              std::span<BYTE> /*dest*/) const override {
    reads++;
    return true;
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    sizes++;
    return size_;
  }

  mutable size_t reads{0};
  mutable size_t sizes{0};

 private:
  std::optional<ULONGLONG> size_;
};

// A reader that overrides nothing optional.
class MinimalReader final : public IDiskReader {
 public:
  bool Open(std::wstring_view /*path*/) override { return true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& /*addr*/,
                              std::span<BYTE> /*dest*/) const override {
    return true;
  }
};

// A path in the temp directory that nothing uses, removed with the object.
struct TempPath final {
  std::filesystem::path path;

  TempPath() {
    std::random_device random_device;
    path = std::filesystem::temp_directory_path() /
           ("ntfsbrowser-size-test-" + std::to_string(random_device()));
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

TEST_CASE("a reader that overrides nothing has no size",
          "[disk-reader][size]") {
  const MinimalReader reader;
  CHECK_FALSE(reader.Size().has_value());
}

TEST_CASE("MemoryDiskReader reports the size of its buffer",
          "[disk-reader][size]") {
  CHECK(Tests::MemoryDiskReader(std::vector<BYTE>(buffer_bytes)).Size() ==
        buffer_bytes);
  CHECK(Tests::MemoryDiskReader({}).Size() == 0);
}

TEST_CASE("SequentialDiskReader has no size", "[disk-reader][size]") {
  const Tests::SequentialDiskReader reader(
      Tests::MakeMemoryProducer(std::vector<BYTE>(buffer_bytes)));
  CHECK_FALSE(reader.Size().has_value());
}

TEST_CASE("PartitionDiskReader reports the length it was given",
          "[disk-reader][size]") {
  CHECK_FALSE(Tests::PartitionDiskReader(partition_offset).Size().has_value());
  CHECK(Tests::PartitionDiskReader(partition_offset, partition_length).Size() ==
        partition_length);
  CHECK(Tests::PartitionDiskReader(0, 0).Size() == 0);
}

TEST_CASE("RecordingDiskReader forwards the size and records nothing",
          "[disk-reader][size]") {
  const TempPath trace;
  auto inner = std::make_unique<CountingReader>(buffer_bytes);
  const CountingReader& counted = *inner;
  const Tests::RecordingDiskReader reader(std::move(inner), trace.path);

  CHECK(reader.Size() == buffer_bytes);
  CHECK(counted.sizes == 1);
  CHECK(counted.reads == 0);
  CHECK(std::filesystem::file_size(trace.path) == 0);
}

TEST_CASE("RecordingDiskReader forwards a missing size",
          "[disk-reader][size]") {
  const TempPath trace;
  const Tests::RecordingDiskReader reader(
      std::make_unique<CountingReader>(std::nullopt), trace.path);

  CHECK_FALSE(reader.Size().has_value());
}

TEST_CASE("ReadAheadDiskReader forwards the size and reads nothing",
          "[disk-reader][size][read-ahead]") {
  auto inner = std::make_unique<CountingReader>(buffer_bytes);
  const CountingReader& counted = *inner;
  const Io::ReadAheadDiskReader reader(std::move(inner));

  CHECK(reader.Size() == buffer_bytes);
  CHECK(counted.sizes == 1);
  CHECK(counted.reads == 0);
}

TEST_CASE("ReadAheadDiskReader forwards a missing size",
          "[disk-reader][size][read-ahead]") {
  const Io::ReadAheadDiskReader over_none(
      std::make_unique<CountingReader>(std::nullopt));
  CHECK_FALSE(over_none.Size().has_value());

  const Io::ReadAheadDiskReader over_nothing(nullptr);
  CHECK_FALSE(over_nothing.Size().has_value());
}

TEMPLATE_TEST_CASE_SIG("FileReader reports the size of its reader",
                       "[disk-reader][size][file-reader]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const Io::FileReader<S> without_reader;
  CHECK_FALSE(without_reader.Size().has_value());

  const Io::FileReader<S> sized(std::make_unique<Tests::MemoryDiskReader>(
      std::vector<BYTE>(buffer_bytes)));
  CHECK(sized.Size() == buffer_bytes);

  const Io::FileReader<S> unsized(std::make_unique<MinimalReader>());
  CHECK_FALSE(unsized.Size().has_value());
}
