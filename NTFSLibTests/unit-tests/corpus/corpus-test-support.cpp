#include "corpus-test-support.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "partition-disk-reader.h"
#include "recording-disk-reader.h"
#include "sequential-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Bits in a DWORD: where a FILETIME's high half starts.
constexpr unsigned dword_bits = 32;

namespace {

// True when the environment variable name is set to a non-empty value.
bool EnvIsSet(const char* name) {
#ifdef _WIN32
  // getenv() is deprecated on MSVC.
  char* value = nullptr;
  std::size_t size = 0;
  const bool is_set =
      _dupenv_s(&value, &size, name) == 0 && value != nullptr && *value != '\0';
  std::free(value);
  return is_set;
#else
  // NOLINTNEXTLINE(concurrency-mt-unsafe): only read, on the test thread.
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0';
#endif
}

// Where a test gets its bytes from.
enum class Source : unsigned char {
  Image,      // Reads the image as is.
  Recording,  // Reads the image, and records a trace of it.
  Trace       // Replays a trace.
};

Source SourceOf(const std::filesystem::path& image) {
  if (EnvIsSet("NTFS_BROWSER_FORCE_TRACES")) {
    return Source::Trace;
  }
  if (!std::filesystem::exists(image)) {
    return Source::Trace;
  }
  return EnvIsSet("NTFS_BROWSER_RECORD_TRACES") ? Source::Recording
                                                : Source::Image;
}

std::filesystem::path TracePath(std::string_view trace_name) {
  return TraceDir() / (std::string(trace_name) + ".trace");
}

// SKIPs the running test when the source it would use does not exist.
void RequireSource(const std::filesystem::path& image,
                   std::string_view trace_name) {
  if (SourceOf(image) != Source::Trace ||
      std::filesystem::exists(TracePath(trace_name))) {
    return;
  }
  SKIP("neither test image " << image.string() << " nor trace "
                             << TracePath(trace_name).string()
                             << " is present");
}

}  // namespace

void RequireCorpusImage(const std::filesystem::path& image,
                        std::string_view trace_name) {
#ifdef NTFS_TEST_REQUIRE_DATA
  if (!EnvIsSet("NTFS_BROWSER_FORCE_TRACES") &&
      !std::filesystem::exists(image)) {
    FAIL("test image not present: " << image.string());
  }
#endif
  RequireSource(image, trace_name);
}

void RequireUnfetchedCorpusImage(const std::filesystem::path& image,
                                 std::string_view trace_name) {
  RequireSource(image, trace_name);
}

void RequireCorpusImage(const std::filesystem::path& image) {
  if (std::filesystem::exists(image)) {
    return;
  }
#ifdef NTFS_TEST_REQUIRE_DATA
  FAIL("test image not present: " << image.string());
#else
  SKIP("test image not present: " << image.string());
#endif
}

std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>
    OpenCorpusImage(const std::filesystem::path& image,
                    ULONGLONG partition_offset, std::string_view trace_name) {
  const Source source = SourceOf(image);
  if (source == Source::Trace) {
    return std::make_unique<SequentialDiskReader>(
        MakeFileStreamProducer(TracePath(trace_name)));
  }

  auto reader = std::make_unique<PartitionDiskReader>(partition_offset);
  REQUIRE(reader->Open(image.wstring()));
  if (source == Source::Recording) {
    return std::make_unique<RecordingDiskReader>(std::move(reader),
                                                 TracePath(trace_name));
  }
  return reader;
}

std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>
    OpenBareVolumeImage(const std::filesystem::path& image_path) {
  auto reader = std::make_unique<PartitionDiskReader>(0);
  REQUIRE(reader->Open(image_path.wstring()));
  return reader;
}

ULONGLONG FileTimeToTicks(const FILETIME& file_time) noexcept {
  return (static_cast<ULONGLONG>(file_time.dwHighDateTime) << dword_bits) |
         file_time.dwLowDateTime;
}

std::tuple<WORD, WORD, WORD>
    FileTimeToDate(const FILETIME& file_time) noexcept {
  // 100 ns ticks per second, and the number of days FILETIME's 1601-01-01
  // epoch precedes civil_from_days()'s 1970-01-01 one (also the seconds
  // Win32 FILETIME<->time_t conversions use: 11644473600 / 86400 = 134774).
  constexpr ULONGLONG ticks_per_second = 10'000'000;
  constexpr ULONGLONG seconds_per_day = 86400;
  constexpr long long epoch_day_offset = 134774;

  const ULONGLONG seconds = FileTimeToTicks(file_time) / ticks_per_second;
  const long long shifted_days =
      static_cast<long long>(seconds / seconds_per_day) - epoch_day_offset;

  // Howard Hinnant's civil_from_days(): days since 1970-01-01 to a
  // proleptic Gregorian (year, month, day), exact for every date FILETIME
  // can represent. See http://howardhinnant.github.io/date_algorithms.html.
  const long long z_adj = shifted_days + 719468;
  const long long era = (z_adj >= 0 ? z_adj : z_adj - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z_adj - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long shifted_year = static_cast<long long>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned month_prime = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * month_prime + 2) / 5 + 1;
  const unsigned month = month_prime < 10 ? month_prime + 3 : month_prime - 9;

  return {static_cast<WORD>(shifted_year + (month <= 2 ? 1 : 0)),
          static_cast<WORD>(month), static_cast<WORD>(day)};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
