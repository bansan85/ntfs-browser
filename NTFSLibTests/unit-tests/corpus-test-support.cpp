#include "corpus-test-support.h"

#include <catch2/catch_test_macros.hpp>

#include "partition-disk-reader.h"

namespace NtfsBrowserTests
{

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

ULONGLONG FileTimeToTicks(const FILETIME& ft) noexcept
{
  return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::tuple<WORD, WORD, WORD> FileTimeToDate(const FILETIME& ft) noexcept
{
  // 100 ns ticks per second, and the number of days FILETIME's 1601-01-01
  // epoch precedes civil_from_days()'s 1970-01-01 one (also the seconds
  // Win32 FILETIME<->time_t conversions use: 11644473600 / 86400 = 134774).
  constexpr ULONGLONG kTicksPerSecond = 10'000'000;
  constexpr ULONGLONG kSecondsPerDay = 86400;
  constexpr long long kEpochDayOffset = 134774;

  const ULONGLONG seconds = FileTimeToTicks(ft) / kTicksPerSecond;
  const long long z =
      static_cast<long long>(seconds / kSecondsPerDay) - kEpochDayOffset;

  // Howard Hinnant's civil_from_days(): days since 1970-01-01 to a
  // proleptic Gregorian (year, month, day), exact for every date FILETIME
  // can represent. See http://howardhinnant.github.io/date_algorithms.html.
  const long long zAdj = z + 719468;
  const long long era = (zAdj >= 0 ? zAdj : zAdj - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(zAdj - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long y = static_cast<long long>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));

  return {static_cast<WORD>(y + (m <= 2 ? 1 : 0)), static_cast<WORD>(m),
          static_cast<WORD>(d)};
}

}  // namespace NtfsBrowserTests
