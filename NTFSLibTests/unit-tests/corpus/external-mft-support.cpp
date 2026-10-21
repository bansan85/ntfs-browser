#include "external-mft-support.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "standalone-mft-disk-reader.h"
#include "test-log-sink.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Ceiling of the FullCache of every scan: 8 MiB, far below the largest $MFT, so
// that a scan proves it does not keep the file.
constexpr std::uint64_t bounded_cache_bytes = std::uint64_t{8} * 1024 * 1024;

// FILETIME ticks (100 ns) in one second.
constexpr std::uint64_t ticks_per_second = 10'000'000;

// FILETIME ticks in one microsecond.
constexpr std::uint64_t ticks_per_microsecond = 10;

// Seconds in a day.
constexpr std::uint64_t seconds_per_day = 86400;

// Days between 1601-01-01 (the FILETIME epoch) and 1970-01-01 (the epoch of
// DaysFromCivil()).
constexpr std::int64_t days_1601_to_1970 = 134774;

// Fields of a listing line: first column, name, four times, size, two owners.
constexpr std::size_t fls_field_count = 9;

// Index of the modification time among them, the first of the four times.
constexpr std::size_t fls_first_time_field = 2;

// Index of the size.
constexpr std::size_t fls_size_field = 6;

// Offset, from UTC, of the time zones the dfir_ntfs listings use. MSK is
// UTC+3 with no daylight saving time, as it was on the 2019 images.
constexpr std::int64_t msk_offset_seconds = 3 * 3600;

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
// days_from_civil, exact for every date a FILETIME holds).
std::int64_t DaysFromCivil(std::int64_t year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// Splits line on tabs.
std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

// "2019-01-20 11:53:36 (UTC)" as seconds since 1601-01-01 UTC. The zero date
// "0000-00-00" of $OrphanFiles is 0.
std::uint64_t ParseListingTime(const std::string& text) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  char zone[8] = {};
  // NOLINTNEXTLINE(cert-err34-c): the format was checked by the caller.
  const int parsed =
      std::sscanf(text.c_str(), "%d-%u-%u %u:%u:%u (%7[A-Z])", &year, &month,
                  &day, &hour, &minute, &second, zone);
  REQUIRE(parsed == 7);
  if (year == 0) {
    return 0;
  }
  std::int64_t offset = 0;
  if (std::string_view(zone) == "MSK") {
    offset = msk_offset_seconds;
  } else {
    REQUIRE(std::string_view(zone) == "UTC");
  }
  const std::int64_t days = DaysFromCivil(year, month, day) + days_1601_to_1970;
  return gsl::narrow<std::uint64_t>(
      days * static_cast<std::int64_t>(seconds_per_day) + hour * 3600 +
      minute * 60 + second - offset);
}

// Text that tells this process from every other one running the tests. It is
// random: a process id would need a platform header, and the 64 bits make a
// collision impossible in practice.
const std::string& ProcessTag() {
  static const std::string tag = [] {
    std::random_device device;
    constexpr unsigned dword_bits = 32;
    const std::uint64_t value =
        (static_cast<std::uint64_t>(device()) << dword_bits) | device();
    char text[17] = {};
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(text);
  }();
  return tag;
}

// Opens file as a standalone $MFT volume of strategy S, and scans it.
template <Cache::Strategy S>
StandaloneMftScan ScanWith(const std::filesystem::path& file,
                           const VolumeOptions& options, bool repair_extent) {
  auto standalone = std::make_unique<StandaloneMftDiskReader>();
  standalone->SetRepairDeclaredExtent(repair_extent);
  const bool opened = standalone->Open(file.wstring());
  INFO("StandaloneMftDiskReader::Open: " << standalone->Failure());
  REQUIRE(opened);
  const DWORD record_size = standalone->FileRecordSize();
  const DWORD cluster_size = standalone->ClusterSize();

  auto counting = std::make_unique<CountingDiskReader>(std::move(standalone));
  const CountingDiskReader* reads = counting.get();
  static_cast<void>(TakeCapturedLog());
  const NtfsVolume<S> volume(std::move(counting), options);
  INFO("log of the volume open: " << TakeCapturedLog());
  REQUIRE(volume.IsVolumeOK());

  StandaloneMftScan scan{.tree = Mft::MftTree(volume),
                         .held_bytes = volume.GetCacheStats().held_bytes,
                         .largest_read = 0,
                         .record_size = record_size,
                         .cluster_size = cluster_size};
  // The scan logs a line per record: do not keep them.
  static_cast<void>(TakeCapturedLog());
  for (const CountingDiskReader::Call& call : reads->Calls()) {
    scan.largest_read = std::max<std::uint64_t>(scan.largest_read, call.length);
  }
  return scan;
}

}  // namespace

TempFile::TempFile(std::span<const BYTE> bytes) {
  // One counter per process: two files of one process never share a name.
  static std::uint64_t counter = 0;
  path_ = std::filesystem::temp_directory_path() /
          ("ntfs-browser-mft-test-" + ProcessTag() + "-" +
           std::to_string(counter++) + ".bin");
  std::ofstream out(path_, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            gsl::narrow<std::streamsize>(bytes.size()));
}

TempFile::~TempFile() {
  std::error_code error;
  std::filesystem::remove(path_, error);
}

StandaloneMftScan ScanStandaloneMft(const std::filesystem::path& file,
                                    Cache::Strategy strategy,
                                    const VolumeOptions& options,
                                    bool repair_extent) {
  return strategy == Cache::Strategy::NoCache
             ? ScanWith<Cache::Strategy::NoCache>(file, options, repair_extent)
             : ScanWith<Cache::Strategy::FullCache>(file, options,
                                                    repair_extent);
}

std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>>
    OpenStandaloneMftVolume(const std::filesystem::path& file,
                            const VolumeOptions& options, bool repair_extent) {
  auto standalone = std::make_unique<StandaloneMftDiskReader>();
  standalone->SetRepairDeclaredExtent(repair_extent);
  const bool opened = standalone->Open(file.wstring());
  INFO("StandaloneMftDiskReader::Open: " << standalone->Failure());
  REQUIRE(opened);
  static_cast<void>(TakeCapturedLog());
  auto volume = std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
      std::move(standalone), options);
  INFO("log of the volume open: " << TakeCapturedLog());
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

VolumeOptions StrictOptions(bool include_deleted) {
  return {.include_deleted = include_deleted,
          .recover_errors = false,
          .max_cache_bytes = bounded_cache_bytes};
}

VolumeOptions RecoveringOptions(bool include_deleted) {
  return {.include_deleted = include_deleted,
          .recover_errors = true,
          .max_cache_bytes = bounded_cache_bytes};
}

std::uint64_t EntryUtcTicks(const FILETIME& local_time) {
#ifdef _WIN32
  FILETIME utc{};
  if (LocalFileTimeToFileTime(&local_time, &utc) != 0) {
    return (static_cast<std::uint64_t>(utc.dwHighDateTime) << 32U) |
           utc.dwLowDateTime;
  }
#endif
  return (static_cast<std::uint64_t>(local_time.dwHighDateTime) << 32U) |
         local_time.dwLowDateTime;
}

std::uint64_t EntryUtcSeconds(const FILETIME& local_time) {
  return EntryUtcTicks(local_time) / ticks_per_second;
}

std::uint64_t UtcTicks(int year, unsigned month, unsigned day, unsigned hour,
                       unsigned minute, unsigned second, unsigned microsecond) {
  const std::int64_t days = DaysFromCivil(year, month, day) + days_1601_to_1970;
  const std::uint64_t seconds = gsl::narrow<std::uint64_t>(
      days * static_cast<std::int64_t>(seconds_per_day) + hour * 3600 +
      minute * 60 + second);
  return seconds * ticks_per_second + microsecond * ticks_per_microsecond;
}

std::vector<FlsLine> ParseFlsListing(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  REQUIRE(in.is_open());
  std::vector<FlsLine> lines;
  std::string text;
  while (std::getline(in, text)) {
    if (!text.empty() && text.back() == '\r') {
      text.pop_back();
    }
    if (text.empty()) {
      continue;
    }
    const std::vector<std::string> fields = SplitTabs(text);
    INFO("listing line: " << text);
    REQUIRE(fields.size() == fls_field_count);

    FlsLine line;
    // "r/r 4-128-1:": the kind, then record-type-id and a colon.
    const std::size_t space = fields[0].find(' ');
    REQUIRE(space != std::string::npos);
    line.kind = fields[0].substr(0, space);
    std::string reference = fields[0].substr(space + 1);
    // A deleted file has a '*' in front of its reference.
    if (reference.starts_with("* ")) {
      line.deleted = true;
      reference.erase(0, 2);
    }
    unsigned long long record = 0;
    unsigned type = 0;
    unsigned id = 0;
    const int parsed =
        std::sscanf(reference.c_str(), "%llu-%u-%u", &record, &type, &id);
    if (parsed == 3) {
      line.record = record;
      line.attribute_type = type;
    } else {
      // "V/V 12305:" has a record number only.
      REQUIRE(std::sscanf(reference.c_str(), "%llu", &record) == 1);
      line.record = record;
    }

    // The stream name follows the last ':' of the last path component.
    std::string name = fields[1];
    const std::size_t last_slash = name.rfind('/');
    const std::size_t from = last_slash == std::string::npos ? 0 : last_slash;
    const std::size_t colon = name.find(':', from);
    if (colon != std::string::npos) {
      line.stream = name.substr(colon + 1);
      name.erase(colon);
    }
    line.path = std::move(name);

    for (std::size_t index = 0; index < line.times.size(); index++) {
      line.times.at(index) =
          ParseListingTime(fields.at(fls_first_time_field + index));
    }
    line.size = std::stoull(fields.at(fls_size_field));
    lines.push_back(std::move(line));
  }
  return lines;
}

std::string ListingPath(const std::wstring& tree_path) {
  std::wstring path = tree_path;
  std::ranges::replace(path, L'\\', L'/');
  if (!path.empty() && path.front() == L'/') {
    path.erase(0, 1);
  }
  return Utf::WideToUtf8(path);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
