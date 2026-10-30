#ifdef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <array>
  #include <cstddef>
  #include <cstdint>
  #include <filesystem>
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

  #include <catch2/catch_test_macros.hpp>
  #include <catch2/generators/catch_generators.hpp>

  #include <ntfs-browser/bench/bench.h>
  #include <ntfs-browser/disk-reader.h>
  #include <ntfs-browser/namespace.h>
  #include <ntfs-browser/tuning/tuning.h>
  #include <ntfs-browser/volume-options.h>

  #include "bench-session-guard.h"
  #include "knob-effect-support.h"
  #include "temp-dir.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::KnobEffect::Tuned;

namespace {

// Bytes of the image file: sixteen blocks of 64 KiB.
constexpr size_t image_bytes = 16 * 65536;

// Bytes of one announced range, and the distance between two of them.
constexpr size_t range_bytes = 4096;
constexpr size_t range_stride = 65536;

constexpr std::array<Tuning::Pattern, Tuning::pattern_count> all_patterns{
    Tuning::Pattern::LargeSequential, Tuning::Pattern::ScatteredGrouped,
    Tuning::Pattern::DependentSmall, Tuning::Pattern::LargeData};

// An image file under the temp folder, and the readers opened on it.
class ImageFile {
 public:
  ImageFile() {
    std::string content(image_bytes, '\0');
    for (size_t i = 0; i < content.size(); i++) {
      content.at(i) = static_cast<char>((i * 7) + (i >> 10U));
    }
    path_ = dir_.Write("image.bin", content);
  }

  [[nodiscard]] std::unique_ptr<IDiskReader>
      Open(const VolumeOptions& options) const {
    auto reader =
        Bench::OpenPlatformReader(path_.wstring(), options.tuning.reader);
    REQUIRE(reader != nullptr);
    return reader;
  }

 private:
  Tests::TempDir dir_;
  std::filesystem::path path_;
};

// Announces count ranges from offset 0, a stride apart.
void Announce(const IDiskReader& reader, size_t count,
              size_t length = range_bytes) {
  for (size_t index = 0; index < count; index++) {
    reader.Prefetch(index * range_stride, length);
  }
}

// The reads ahead started by announcing count ranges to a reader with the
// options' caps, and nothing else.
std::uint64_t StartedBy(const VolumeOptions& options, size_t count,
                        size_t length = range_bytes,
                        std::optional<Tuning::Pattern> pattern = std::nullopt) {
  const ImageFile image;
  const std::unique_ptr<IDiskReader> reader = image.Open(options);
  Tests::BenchSessionGuard session;
  if (pattern) {
    reader->SetReadPattern(*pattern);
  }
  Announce(*reader, count, length);
  return session.Finish().disk.prefetch_reads;
}

}  // namespace

TEST_CASE("reader.max_reads sets the reads a Win32DiskReader keeps in flight",
          "[tuning][knob-effect][reader]") {
  const std::uint64_t reads =
      GENERATE(std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3});
  INFO("max_reads " << reads);

  CHECK(StartedBy(Tuned({}, {{"reader.max_reads", reads}}), 4) == reads);
  CHECK(StartedBy({}, 4) == 4);
}

TEST_CASE("reader.max_bytes sets the bytes a Win32DiskReader holds ahead",
          "[tuning][knob-effect][reader]") {
  const std::uint64_t bytes =
      GENERATE(std::uint64_t{4096}, std::uint64_t{8192});
  INFO("max_bytes " << bytes);

  // The first read always starts; the others while the total fits.
  CHECK(StartedBy(Tuned({}, {{"reader.max_bytes", bytes}}), 4) ==
        bytes / range_bytes);
}

TEST_CASE("reader.max_range_bytes sets the longest range that is announced",
          "[tuning][knob-effect][reader]") {
  const VolumeOptions tight = Tuned({}, {{"reader.max_range_bytes", 4096}});

  CHECK(StartedBy(tight, 1, 8192) == 0);
  CHECK(StartedBy(tight, 1, 4096) == 1);
  CHECK(StartedBy({}, 1, 8192) == 1);
}

TEST_CASE(
    "reader.max_announced_ranges sets the ranges a reader keeps announced",
    "[tuning][knob-effect][reader]") {
  const std::uint64_t ranges = GENERATE(std::uint64_t{1}, std::uint64_t{2});
  INFO("max_announced_ranges " << ranges);

  // A hint past the cap is dropped, and its read never starts.
  CHECK(StartedBy(Tuned({}, {{"reader.max_announced_ranges", ranges}}), 4) ==
        ranges);
}

TEST_CASE(
    "reader.match_lookahead sets the announced ranges a read is matched to",
    "[tuning][knob-effect][reader]") {
  const auto plain_reads = [](const VolumeOptions& options) {
    const ImageFile image;
    const std::unique_ptr<IDiskReader> reader = image.Open(options);
    Tests::BenchSessionGuard session;
    Announce(*reader, 4);
    std::vector<BYTE> buffer(range_bytes);
    LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(2 * range_stride)};
    REQUIRE(reader->ReadInto(addr, buffer));
    const Bench::Report report = session.Finish();
    return report.disk.reads - report.disk.prefetch_reads;
  };

  // The third range is served from the read ahead, unless the reader looks at
  // two ranges only: then the read goes to the disk by itself.
  CHECK(plain_reads({}) == 0);
  CHECK(plain_reads(Tuned({}, {{"reader.match_lookahead", 2}})) == 1);
  CHECK(plain_reads(Tuned({}, {{"reader.match_lookahead", 3}})) == 0);
}

TEST_CASE("A per-pattern reader knob applies to its pattern only",
          "[tuning][knob-effect][reader]") {
  const VolumeOptions m2_narrow =
      Tuned({}, {{"reader.max_reads", 1, Tuning::Pattern::ScatteredGrouped}});

  // A reader starts in the first pattern, whose cap is not that of M2.
  CHECK(StartedBy(m2_narrow, 4) == 4);
  CHECK(StartedBy(m2_narrow, 4, range_bytes,
                  Tuning::Pattern::LargeSequential) == 4);
  CHECK(StartedBy(m2_narrow, 4, range_bytes,
                  Tuning::Pattern::ScatteredGrouped) == 1);
  CHECK(StartedBy(m2_narrow, 4, range_bytes, Tuning::Pattern::LargeData) == 4);
}

TEST_CASE("SetReadPattern switches the caps of a Win32DiskReader",
          "[tuning][knob-effect][reader]") {
  const VolumeOptions m1_narrow =
      Tuned({}, {{"reader.max_reads", 1, Tuning::Pattern::LargeSequential}});
  const ImageFile image;

  SECTION("a wider pattern starts the ranges that waited") {
    const std::unique_ptr<IDiskReader> reader = image.Open(m1_narrow);
    Tests::BenchSessionGuard session;
    Announce(*reader, 4);
    reader->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
    CHECK(session.Finish().disk.prefetch_reads == 4);
  }

  SECTION("the same pattern again starts nothing more") {
    const std::unique_ptr<IDiskReader> reader = image.Open(m1_narrow);
    Tests::BenchSessionGuard session;
    Announce(*reader, 4);
    reader->SetReadPattern(Tuning::Pattern::LargeSequential);
    CHECK(session.Finish().disk.prefetch_reads == 1);
  }

  SECTION("a narrower pattern lets the reads in flight finish") {
    const std::unique_ptr<IDiskReader> reader = image.Open(
        Tuned({}, {{"reader.max_reads", 1, Tuning::Pattern::LargeData}}));
    Tests::BenchSessionGuard session;
    Announce(*reader, 3);
    reader->SetReadPattern(Tuning::Pattern::LargeData);
    reader->Prefetch(3 * range_stride, range_bytes);
    // The three started stay, and the fourth waits under the cap of one.
    CHECK(session.Finish().disk.prefetch_reads == 3);
  }
}

TEST_CASE("The caps of every pattern come from the options of the volume",
          "[tuning][knob-effect][reader]") {
  VolumeOptions options;
  for (const Tuning::Pattern pattern : all_patterns) {
    const std::uint64_t reads = 1 + static_cast<std::uint64_t>(pattern);
    options = Tuned(options, {{"reader.max_reads", reads, pattern}});
  }

  for (const Tuning::Pattern pattern : all_patterns) {
    INFO("pattern " << Tuning::PatternName(pattern));
    CHECK(StartedBy(options, 6, range_bytes, pattern) ==
          1 + static_cast<std::uint64_t>(pattern));
  }
}

#endif
