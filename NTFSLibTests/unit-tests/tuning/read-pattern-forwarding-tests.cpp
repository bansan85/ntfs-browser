#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "counting-disk-reader.h"
#include "memory-disk-reader.h"
#include "recording-disk-reader.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using Kind = Tests::CountingDiskReader::Kind;

namespace {

// The calls a ProbeDiskReader received, in order.
struct Seen {
  std::vector<Tuning::Pattern> patterns;
  size_t reads{0};
  size_t hints{0};
};

// A reader over memory that notes the patterns it is told, into a Seen the
// test owns.
class ProbeDiskReader final : public IDiskReader {
 public:
  ProbeDiskReader(std::vector<BYTE> image, Seen& seen)
      : inner_(std::move(image)), seen_(&seen) {}

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    seen_->reads++;
    return inner_.ReadInto(addr, dest);
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return inner_.Size();
  }

  void Prefetch(ULONGLONG /*offset*/, ULONGLONG /*length*/) const override {
    seen_->hints++;
  }

  void SetReadPattern(Tuning::Pattern pattern) const override {
    seen_->patterns.push_back(pattern);
  }

 private:
  Tests::MemoryDiskReader inner_;
  Seen* seen_;
};

constexpr size_t image_size = 8192;

std::vector<Tuning::Pattern> AllPatterns() {
  return {Tuning::Pattern::LargeSequential, Tuning::Pattern::DependentSmall,
          Tuning::Pattern::LargeData, Tuning::Pattern::ScatteredGrouped,
          Tuning::Pattern::LargeSequential};
}

std::filesystem::path TempTrace() {
  std::random_device random_device;
  return std::filesystem::temp_directory_path() /
         ("ntfsbrowser-pattern-trace-" + std::to_string(random_device()) +
          ".trace");
}

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Removes the file when it goes out of scope.
struct TempFile final {
  std::filesystem::path path = TempTrace();

  TempFile() = default;
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

}  // namespace

TEST_CASE("CountingDiskReader passes a read pattern on and does not record it",
          "[tuning][invariance][disk-reader]") {
  Seen seen;
  Tests::CountingDiskReader counting(
      std::make_unique<ProbeDiskReader>(std::vector<BYTE>(image_size), seen));

  std::array<BYTE, 16> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  for (const Tuning::Pattern pattern : AllPatterns()) {
    counting.SetReadPattern(pattern);
  }
  REQUIRE(counting.ReadInto(addr, dest));
  counting.Prefetch(0, 16);
  counting.CancelPrefetch();

  CHECK(seen.patterns == AllPatterns());
  const std::vector<Tests::CountingDiskReader::Call>& calls = counting.Calls();
  REQUIRE(calls.size() == 3);
  CHECK(calls.at(0).kind == Kind::Read);
  CHECK(calls.at(1).kind == Kind::Prefetch);
  CHECK(calls.at(2).kind == Kind::CancelPrefetch);
  CHECK(counting.ReadCount() == 1);
}

TEST_CASE("RecordingDiskReader passes a read pattern on and traces nothing",
          "[tuning][invariance][disk-reader]") {
  std::vector<BYTE> image(image_size);
  for (size_t i = 0; i < image.size(); i++) {
    // i < image.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    image[i] = static_cast<BYTE>(i * 3);
  }

  const auto trace_of = [&](bool with_patterns, Seen& seen) {
    const TempFile trace;
    {
      Tests::RecordingDiskReader recording(
          std::make_unique<ProbeDiskReader>(image, seen), trace.path);
      std::array<BYTE, 64> dest{};
      LARGE_INTEGER addr{.QuadPart = 100};
      if (with_patterns) {
        for (const Tuning::Pattern pattern : AllPatterns()) {
          recording.SetReadPattern(pattern);
        }
      }
      REQUIRE(recording.ReadInto(addr, dest));
      if (with_patterns) {
        recording.SetReadPattern(Tuning::Pattern::LargeData);
      }
      addr.QuadPart = 3000;
      REQUIRE(recording.ReadInto(addr, dest));
    }
    return ReadFileBytes(trace.path);
  };

  Seen without;
  Seen with;
  const std::vector<char> plain = trace_of(false, without);
  const std::vector<char> patterned = trace_of(true, with);

  CHECK(plain.size() == 128);
  CHECK(patterned == plain);
  CHECK(without.patterns.empty());
  std::vector<Tuning::Pattern> expected = AllPatterns();
  expected.push_back(Tuning::Pattern::LargeData);
  CHECK(with.patterns == expected);
}

TEST_CASE("The decorators pass the media kind of the reader they wrap",
          "[tuning][invariance][disk-reader]") {
  const std::array<Tuning::MediaKind, 3> kinds{Tuning::MediaKind::Unknown,
                                               Tuning::MediaKind::SolidState,
                                               Tuning::MediaKind::Rotational};
  for (const Tuning::MediaKind kind : kinds) {
    const auto make = [&]() {
      auto memory =
          std::make_unique<Tests::MemoryDiskReader>(std::vector<BYTE>(16));
      memory->SetMediaKind(kind);
      return memory;
    };

    const Tests::CountingDiskReader counting(make());
    CHECK(counting.GetMediaKind() == kind);
    // Asking reads nothing.
    CHECK(counting.Calls().empty());

    TempFile trace;
    const Tests::RecordingDiskReader recording(make(), trace.path);
    CHECK(recording.GetMediaKind() == kind);

    const Io::ReadAheadDiskReader read_ahead(make());
    CHECK(read_ahead.GetMediaKind() == kind);
  }
  CHECK(Tests::MemoryDiskReader(std::vector<BYTE>(16)).GetMediaKind() ==
        Tuning::MediaKind::Unknown);
}
