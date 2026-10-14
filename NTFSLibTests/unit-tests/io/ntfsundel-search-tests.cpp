#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfsundel-core.h"

namespace {

namespace Cache = NtfsBrowser::Cache;
using FullVolume = NtfsBrowser::NtfsVolume<Cache::Strategy::FullCache>;
using NtfsUndel::DeletedFile;
using NtfsUndel::SearchControl;
using NtfsUndel::SearchResult;

// $STANDARD_INFORMATION alter time the fake image writes into every record.
constexpr std::uint64_t fake_write_time = 0x1112131415161718ULL;

// fake_write_time as GetFileTime reports it: converted to local time.
std::uint64_t LocalFakeWriteTime() {
  const FILETIME utc{
      .dwLowDateTime = static_cast<DWORD>(fake_write_time & 0xFFFFFFFFU),
      .dwHighDateTime = static_cast<DWORD>(fake_write_time >> 32U)};
  FILETIME local{};
  REQUIRE(FileTimeToLocalFileTime(&utc, &local) != 0);
  return (static_cast<std::uint64_t>(local.dwHighDateTime) << 32U) |
         local.dwLowDateTime;
}

// A volume over image. Freed records are visible only with include_deleted.
std::unique_ptr<FullVolume> OpenVolume(std::vector<BYTE> image,
                                       bool include_deleted = true) {
  return std::make_unique<FullVolume>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)),
      NtfsBrowser::VolumeOptions{.include_deleted = include_deleted});
}

// Runs a search and checks it was not rejected.
SearchResult Search(FullVolume& volume, std::wstring_view filter,
                    const SearchControl& control = {}) {
  std::optional<SearchResult> result =
      NtfsUndel::SearchDeletedFiles(volume, filter, control);
  REQUIRE(result.has_value());
  return *std::move(result);
}

// The record numbers of the files a search listed, in order.
std::vector<std::uint64_t> References(const SearchResult& result) {
  std::vector<std::uint64_t> references;
  for (const DeletedFile& file : result.files) {
    references.push_back(file.reference);
  }
  return references;
}

// The names of the files a search listed, in order.
std::vector<std::wstring> Names(const SearchResult& result) {
  std::vector<std::wstring> names;
  for (const DeletedFile& file : result.files) {
    names.push_back(file.name);
  }
  return names;
}

}  // namespace

TEST_CASE("NtfsUndel search lists freed records in record number order",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = Search(*volume, L".*");

  // Live, empty and nameless records are skipped. A freed
  // directory counts too, since only the freed flag decides.
  CHECK(References(result) == std::vector<std::uint64_t>{19, 20, 21, 22});
  CHECK(Names(result) == std::vector<std::wstring>{L"old.tmp", L"OldDir",
                                                   L"draft.doc", L"stale.txt"});
  CHECK_FALSE(result.truncated);
}

TEST_CASE("NtfsUndel search builds each path from the directory entries",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = Search(*volume, L".*");

  REQUIRE(result.files.size() == 4);
  // The root is named "." on a real volume, and the walk takes that name as
  // the first component. Its index names Docs, so the path is "." then Docs.
  CHECK(result.files[0].full_path == L".\\Docs\\old.tmp");
  CHECK(result.files[1].full_path == L".\\Docs\\OldDir");
  // No directory index names draft.doc or stale.txt: each keeps its own name.
  CHECK(result.files[2].full_path == L"draft.doc");
  CHECK(result.files[3].full_path == L"stale.txt");
}

TEST_CASE("NtfsUndel search reports the write time of each freed record",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = Search(*volume, L".*");

  REQUIRE(result.files.size() == 4);
  for (const DeletedFile& file : result.files) {
    CHECK(file.write_time == LocalFakeWriteTime());
  }
}

TEST_CASE("NtfsUndel search matches the whole name as a regex",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  CHECK(References(Search(*volume, L"old\\.tmp")) ==
        std::vector<std::uint64_t>{19});
  CHECK(References(Search(*volume, L"OldDir|stale\\.txt")) ==
        std::vector<std::uint64_t>{20, 22});
  // A partial name does not match: the regex must cover the whole name.
  CHECK(Search(*volume, L"old").files.empty());
}

TEST_CASE("NtfsUndel search trims blanks around the filter",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  CHECK(References(Search(*volume, L"  stale\\.txt \t")) ==
        std::vector<std::uint64_t>{22});
}

TEST_CASE("NtfsUndel search with only blanks as filter matches nothing",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  // Blanks are trimmed to an empty pattern, which only an empty name matches.
  // Records without a name are never candidates.
  CHECK(Search(*volume, L"   ").files.empty());
}

TEST_CASE("NtfsUndel search rejects a filter that is not a regex",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  CHECK_FALSE(NtfsUndel::SearchDeletedFiles(*volume, L"[", {}).has_value());
  CHECK_FALSE(NtfsUndel::SearchDeletedFiles(*volume, L"(abc", {}).has_value());
}

TEST_CASE("NtfsUndel search does not scan records past max_records",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  // Record 19 is still scanned. Records 20 and up are not.
  const SearchControl control{.max_records = 20};

  CHECK(References(Search(*volume, L".*", control)) ==
        std::vector<std::uint64_t>{19});
}

TEST_CASE("NtfsUndel search stops at once when stop is requested",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  const SearchControl control{.stop_requested = [] { return true; }};

  // A stopped scan still returns a result, just with no records in it.
  CHECK(Search(*volume, L".*", control).files.empty());
}

TEST_CASE("NtfsUndel search truncates the list at max_listed",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = Search(*volume, L".*", {.max_listed = 2});

  CHECK(References(result) == std::vector<std::uint64_t>{19, 20});
  CHECK(result.truncated);
}

TEST_CASE("NtfsUndel search flags truncation when the list fills exactly",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree());

  // Four freed files fill a list of four. The flag still says the list was
  // cut, as the dialog warns in that case.
  const SearchResult result = Search(*volume, L".*", {.max_listed = 4});

  CHECK(result.files.size() == 4);
  CHECK(result.truncated);
}

TEST_CASE("NtfsUndel search lists nothing without include_deleted",
          "[undel][search][characterization]") {
  const auto volume =
      OpenVolume(NtfsBrowserTests::BuildFakeNtfsImageWithFreedFileTree(),
                 /*include_deleted=*/false);

  CHECK(Search(*volume, L".*").files.empty());
}

TEST_CASE(
    "NtfsUndel FormatWriteTime prints a FILETIME as its date and time without "
    "conversion",
    "[undel][search][characterization]") {
  // 2000-01-01 00:00 UTC, in FILETIME ticks.
  CHECK(NtfsUndel::FormatWriteTime(125'911'584'000'000'000ULL) ==
        L"2000-01-01  00:00");
  // The FILETIME epoch.
  CHECK(NtfsUndel::FormatWriteTime(0) == L"1601-01-01  00:00");
}

TEST_CASE("NtfsUndel FormatWriteTime prints zeros for an unconvertible time",
          "[undel][search][characterization]") {
  CHECK(NtfsUndel::FormatWriteTime(~0ULL) == L"0000-00-00  00:00");
}
