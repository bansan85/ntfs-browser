#include "corpus-test-support.h"

#include <ntfs-browser/win-types.h>

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "partition-disk-reader.h"

namespace NtfsBrowser
{
class IDiskReader;
}  // namespace NtfsBrowser

namespace NtfsBrowserTests
{

// Bits in a DWORD: where a FILETIME's high half starts.
constexpr unsigned kDwordBits = 32;

void RequireCorpusImage(const std::filesystem::path& image)
{
  if (std::filesystem::exists(image))
  {
    return;
  }
#ifdef NTFS_TEST_REQUIRE_DATA
  FAIL("test image not present: " << image.string());
#else
  SKIP("test image not present: " << image.string());
#endif
}

std::unique_ptr<NtfsBrowser::IDiskReader>
    OpenBareVolumeImage(const std::filesystem::path& imagePath)
{
  auto reader = std::make_unique<PartitionDiskReader>(0);
  REQUIRE(reader->Open(imagePath.wstring()));
  return reader;
}

ULONGLONG FileTimeToTicks(const FILETIME& file_time) noexcept
{
  return (static_cast<ULONGLONG>(file_time.dwHighDateTime) << kDwordBits) |
         file_time.dwLowDateTime;
}

std::tuple<WORD, WORD, WORD> FileTimeToDate(const FILETIME& file_time) noexcept
{
  // 100 ns ticks per second, and the number of days FILETIME's 1601-01-01
  // epoch precedes civil_from_days()'s 1970-01-01 one (also the seconds
  // Win32 FILETIME<->time_t conversions use: 11644473600 / 86400 = 134774).
  constexpr ULONGLONG kTicksPerSecond = 10'000'000;
  constexpr ULONGLONG kSecondsPerDay = 86400;
  constexpr long long kEpochDayOffset = 134774;

  const ULONGLONG seconds = FileTimeToTicks(file_time) / kTicksPerSecond;
  const long long shifted_days =
      static_cast<long long>(seconds / kSecondsPerDay) - kEpochDayOffset;

  // Howard Hinnant's civil_from_days(): days since 1970-01-01 to a
  // proleptic Gregorian (year, month, day), exact for every date FILETIME
  // can represent. See http://howardhinnant.github.io/date_algorithms.html.
  const long long zAdj = shifted_days + 719468;
  const long long era = (zAdj >= 0 ? zAdj : zAdj - 146096) / 146097;
  const auto doe = static_cast<unsigned>(zAdj - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long shifted_year = static_cast<long long>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned month_prime = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * month_prime + 2) / 5 + 1;
  const unsigned month =
      month_prime + (month_prime < 10 ? 3 : static_cast<unsigned>(-9));

  return {static_cast<WORD>(shifted_year + (month <= 2 ? 1 : 0)),
          static_cast<WORD>(month), static_cast<WORD>(day)};
}

}  // namespace NtfsBrowserTests
