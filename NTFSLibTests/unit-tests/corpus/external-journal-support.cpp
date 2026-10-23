#include "external-journal-support.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "external-journal-samples.h"
#include "external-mft-support.h"
#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace NTFS_BROWSER_NAMESPACE {

struct VolumeOptions;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests::RealJournal {

namespace {

// Ticks (100 ns) in one second, to turn UtcTicks() into whole seconds.
constexpr std::uint64_t ticks_per_second = 10'000'000;

// Hexadecimal digits of one half of a 128-bit file ID: 64 bits.
constexpr std::size_t id_half_digits = 16;

// Hexadecimal digits of a 128-bit file ID.
constexpr std::size_t id_digits = 2 * id_half_digits;

// Label of the dfir_ntfs journal, which the tests also look up by name. It is
// the only fixture whose stream holds V4 records.
constexpr std::string_view dfir_label = "dfir_ntfs";

std::uint64_t ParseUnsigned(std::string_view text, int base = 10) {
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, base);
  INFO("value: " << text);
  REQUIRE(error == std::errc{});
  REQUIRE(end == text.data() + text.size());
  return value;
}

// "0x00000100: File create" as 0x100. The names after the colon are ignored.
DWORD ParseHexFlags(std::string_view text) {
  REQUIRE(text.starts_with("0x"));
  const std::size_t colon = text.find(':');
  REQUIRE(colon != std::string_view::npos);
  return static_cast<DWORD>(
      ParseUnsigned(text.substr(2, colon - 2), /*base=*/16));
}

// The two halves of a 32-digit hexadecimal ID, the high one first.
void ParseFileId(std::string_view text, ULONGLONG& low, ULONGLONG& high) {
  REQUIRE(text.size() == id_digits);
  high = ParseUnsigned(text.substr(0, id_half_digits), /*base=*/16);
  low = ParseUnsigned(text.substr(id_half_digits), /*base=*/16);
}

// "1/22/2019 21:36:10" as whole seconds since 1601-01-01, read as UTC.
std::uint64_t ParseTimestamp(const std::string& text) {
  int month = 0;
  unsigned day = 0;
  int year = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  // NOLINTNEXTLINE(cert-err34-c): the count of parsed fields is checked.
  const int parsed = std::sscanf(text.c_str(), "%d/%u/%d %u:%u:%u", &month,
                                 &day, &year, &hour, &minute, &second);
  INFO("time stamp: " << text);
  REQUIRE(parsed == 6);
  return UtcTicks({.year = year,
                   .month = static_cast<unsigned>(month),
                   .day = day,
                   .hour = hour,
                   .minute = minute,
                   .second = second}) /
         ticks_per_second;
}

}  // namespace

const std::array<Fixture, 5>& Fixtures() {
  static const std::array<Fixture, 5> fixtures = {{
      {.label = "Server 2012 R2",
       .project = Project::DfirMuseum,
       .relative_path = "Windows/NTFSArtifacts/J/Win2012R2/StolenSzechuan/$J",
       .file_size = 9219384,
       .record_count = 82151,
       .range_record_count = 0,
       .base_usn = 0,
       .samples = stolen_samples},
      {.label = "Windows 10",
       .project = Project::DfirMuseum,
       .relative_path =
           "Windows/NTFSArtifacts/J/Win10/BelkasoftCTF_InisderThreat/$J",
       .file_size = 40'355'304,
       .record_count = 272175,
       .range_record_count = 0,
       .base_usn = 92'274'688,
       .samples = belkasoft_samples},
      {.label = "Windows 11",
       .project = Project::DfirMuseum,
       .relative_path = "Windows/NTFSArtifacts/J/Win11/RathbunVM/$J",
       .file_size = 16'565'704,
       .record_count = 133099,
       .range_record_count = 0,
       .base_usn = 0,
       .samples = win11_samples},
      {.label = "Server 2022",
       .project = Project::DfirMuseum,
       .relative_path = "Windows/NTFSArtifacts/J/Server2022/APTSimulatorVM/$J",
       .file_size = 6088784,
       .record_count = 57738,
       .range_record_count = 0,
       .base_usn = 0,
       .samples = server2022_samples},
      {.label = dfir_label,
       .project = Project::DfirNtfs,
       .relative_path = "test_data/usnjrnlj.bin",
       .file_size = 30056,
       .record_count = 264,
       .range_record_count = 7,
       .base_usn = 0,
       .samples = dfir_samples},
  }};
  return fixtures;
}

const Fixture& FixtureNamed(std::string_view label) {
  const auto& all = Fixtures();
  const auto found = std::ranges::find(all, label, &Fixture::label);
  REQUIRE(found != all.end());
  return *found;
}

std::filesystem::path RequirePathOf(const Fixture& fixture) {
  const std::filesystem::path& root =
      fixture.project == Project::DfirNtfs ? DfirNtfsDir() : DfirMuseumDir();
  return RequireCorpusFile(root / std::u8string(fixture.relative_path.begin(),
                                                fixture.relative_path.end()));
}

std::optional<Usn::Journal> ReadJournalOf(std::span<const BYTE> stream,
                                          ULONGLONG sparse_head_bytes,
                                          Cache::Strategy strategy,
                                          const VolumeOptions& options) {
  FakeUsnOptions journal_options{.sparse_head_bytes = sparse_head_bytes};
  journal_options.raw_stream.assign(stream.begin(), stream.end());
  std::vector<BYTE> image = UsnSupport::JournalImage(journal_options);

  const auto read = [&]<Cache::Strategy S>() -> std::optional<Usn::Journal> {
    const auto opened = UsnSupport::OpenVolume<S>(std::move(image), options);
    const Usn::Journal* journal = opened.volume->GetUsnJournal();
    if (journal == nullptr) {
      return std::nullopt;
    }
    return *journal;
  };
  return strategy == Cache::Strategy::NoCache
             ? read.template operator()<Cache::Strategy::NoCache>()
             : read.template operator()<Cache::Strategy::FullCache>();
}

FsutilListing ParseFsutilListing(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  REQUIRE(in.is_open());

  FsutilListing listing;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    // A blank line ends a block, and an extent list (V4) is indented.
    if (line.empty() || line.front() == ' ') {
      continue;
    }
    const std::size_t colon = line.find(':');
    INFO("line: " << line);
    REQUIRE(colon != std::string::npos);
    std::string key = line.substr(0, colon);
    key.erase(key.find_last_not_of(' ') + 1);
    std::string_view value = std::string_view(line).substr(colon + 1);
    value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));

    if (key == "Usn") {
      listing.records.emplace_back().usn =
          static_cast<LONGLONG>(ParseUnsigned(value));
    } else if (listing.records.empty()) {
      if (key == "Next USN") {
        listing.next_usn = ParseUnsigned(value);
      }
    } else {
      FsutilRecord& record = listing.records.back();
      if (key == "File name") {
        record.name = std::string(value);
        record.has_name = true;
      } else if (key == "File name length") {
        record.name_length = ParseUnsigned(value);
      } else if (key == "Reason") {
        record.reason = ParseHexFlags(value);
      } else if (key == "Time stamp") {
        record.seconds = ParseTimestamp(std::string(value));
      } else if (key == "File attributes") {
        record.attributes = ParseHexFlags(value);
      } else if (key == "File ID") {
        ParseFileId(value, record.file_id_low, record.file_id_high);
      } else if (key == "Parent file ID") {
        ParseFileId(value, record.parent_id_low, record.parent_id_high);
      } else if (key == "Major version") {
        record.major_version = static_cast<unsigned>(ParseUnsigned(value));
      }
    }
  }
  return listing;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::RealJournal
