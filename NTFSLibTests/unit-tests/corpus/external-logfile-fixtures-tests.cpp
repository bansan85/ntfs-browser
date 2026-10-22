#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"

namespace {

using NTFS_BROWSER_NAMESPACE::Tests::DfirMuseumDir;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile;

// Bytes of a signature: the first four of a $LogFile page.
using Signature = std::array<BYTE, 4>;

// "RSTR": a restart area, the first page of a log that was ever used.
constexpr Signature restart_signature = {'R', 'S', 'T', 'R'};

// "RCRD": a record page. rcrd_dax.bin is a single one.
constexpr Signature record_signature = {'R', 'C', 'R', 'D'};

// 0xFFFFFFFF: a log that was reset, as `chkdsk` leaves it. No RSTR page yet.
constexpr Signature reset_signature = {0xFF, 0xFF, 0xFF, 0xFF};

// Which kept project holds a fixture.
enum class Project : unsigned char { DfirNtfs, DfirMuseum };

// One kept $LogFile fixture: its path under its project's folder, its size
// and the signature of its first page. The sizes come from the selection
// report. A $LogFile parser is a later feature: nothing else is checked.
struct LogFileFixture {
  Project project;
  std::string_view relative_path;
  std::uintmax_t size;
  Signature signature;
};

constexpr std::array<LogFileFixture, 9> log_file_fixtures = {{
    {Project::DfirNtfs, "test_data/LogFile_7.bin", 172032, restart_signature},
    {Project::DfirNtfs, "test_data/LogFile_10.bin", 212992, restart_signature},
    {Project::DfirNtfs, "test_data/LogFile_10_downgraded.bin", 212992,
     restart_signature},
    {Project::DfirNtfs, "test_data/LogFile_10_large.bin", 225280,
     restart_signature},
    {Project::DfirNtfs, "test_data/LogFile_empty.bin", 32768, reset_signature},
    {Project::DfirNtfs, "test_data/rcrd_dax.bin", 448, record_signature},
    {Project::DfirMuseum,
     "Windows/NTFSArtifacts/LogFile/Win2012R2/StolenSzechuan/$LogFile",
     18'317'312, restart_signature},
    {Project::DfirMuseum,
     "Windows/NTFSArtifacts/LogFile/Win10/BelkasoftCTF_InsiderThreat/$LogFile",
     56'639'488, restart_signature},
    {Project::DfirMuseum,
     "Windows/NTFSArtifacts/LogFile/Win11/RathbunVM/$LogFile", 67'108'864,
     restart_signature},
}};

std::filesystem::path PathOf(const LogFileFixture& fixture) {
  const std::filesystem::path& root =
      fixture.project == Project::DfirNtfs ? DfirNtfsDir() : DfirMuseumDir();
  return root / std::u8string(fixture.relative_path.begin(),
                              fixture.relative_path.end());
}

}  // namespace

TEST_CASE("Every $LogFile fixture is present and starts with its signature",
          "[dfir-corpora][integration][logfile]") {
  const LogFileFixture fixture = GENERATE(from_range(log_file_fixtures));
  CAPTURE(fixture.relative_path);
  const std::filesystem::path file = RequireCorpusFile(PathOf(fixture));

  CHECK(std::filesystem::file_size(file) == fixture.size);

  std::ifstream in(file, std::ios::binary);
  REQUIRE(in.is_open());
  Signature first_bytes{};
  in.read(reinterpret_cast<char*>(first_bytes.data()),
          gsl::narrow<std::streamsize>(first_bytes.size()));
  REQUIRE(gsl::narrow<std::size_t>(in.gcount()) == first_bytes.size());
  CHECK(first_bytes == fixture.signature);
}

TEST_CASE("ReadCorpusFile returns every byte of a small fixture",
          "[dfir-corpora][integration][logfile]") {
  const std::filesystem::path file =
      RequireCorpusFile(DfirNtfsDir() / "test_data" / "rcrd_dax.bin");

  const std::vector<BYTE> bytes = ReadCorpusFile(file);

  REQUIRE(bytes.size() == 448);
  CHECK(Signature{bytes[0], bytes[1], bytes[2], bytes[3]} == record_signature);
}
