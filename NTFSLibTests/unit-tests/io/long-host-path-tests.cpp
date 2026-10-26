#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "log/ntfs-common.h"
#include "long-path-support.h"
#include "test-log-sink.h"

#ifdef _WIN32
  #include "io/file-reader.h"
#endif

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage;
using NTFS_BROWSER_NAMESPACE::Tests::LongTempDir;

#ifdef _WIN32

namespace {

// The ways a user can write the path of a file, on Windows: as it is, with
// forward slashes, with the extended-length prefix, and with a "." in it.
std::vector<std::wstring> FormsOf(const Fs::path& file) {
  const std::wstring plain = file.wstring();
  std::wstring forward = plain;
  std::ranges::replace(forward, L'\\', L'/');
  return {plain, forward, LR"(\\?\)" + plain,
          file.parent_path().wstring() + LR"(\.\)" + file.filename().wstring()};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "Win32DiskReader opens a file at a path of more than 260 units",
    "[io][long-path]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const LongTempDir directory(300);
  const std::vector<BYTE> image = BuildFakeNtfsImage();
  const Fs::path file = directory.Write("image.img", image);
  REQUIRE(file.native().size() > 260);

  for (const std::wstring& form : FormsOf(file)) {
    INFO(Fs::path(form).string().substr(0, 60));
    NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;
    REQUIRE(reader.Open(form));

    std::array<BYTE, 512> sector{};
    LARGE_INTEGER address{};
    address.QuadPart = 0;
    CHECK(reader.ReadInto(address, sector));
    CHECK(std::equal(sector.begin(), sector.end(), image.begin()));
    CHECK(reader.Size() == image.size());
  }
}

TEMPLATE_TEST_CASE_SIG("Win32DiskReader reads ahead from a file at a long path",
                       "[io][long-path]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const LongTempDir directory(300);
  const std::vector<BYTE> image = BuildFakeNtfsImage();
  const Fs::path file = directory.Write("image.img", image);
  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;
  REQUIRE(reader.Open(file.wstring()));

  // The second handle of the read ahead opens the same long path.
  reader.Prefetch(4096, 4096);
  std::array<BYTE, 4096> block{};
  LARGE_INTEGER address{};
  address.QuadPart = 4096;
  CHECK(reader.ReadInto(address, block));
  CHECK(std::equal(block.begin(), block.end(), image.begin() + 4096));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume opens an image at a path of more than 260 "
    "units",
    "[io][long-path]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const LongTempDir directory(300);
  const Fs::path file = directory.Write(
      "image.img",
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree());

  for (const std::wstring& form : FormsOf(file)) {
    INFO(Fs::path(form).string().substr(0, 60));
    const NTFS_BROWSER_NAMESPACE::NtfsVolume<S> volume{std::wstring_view(form)};

    CHECK(volume.IsVolumeOK());
  }
}

TEST_CASE("NtfsVolume reports a long path that does not exist",
          "[io][long-path]") {
  const LongTempDir directory(300);
  const Fs::path file = directory.FileIn("missing.img");

  const NTFS_BROWSER_NAMESPACE::NtfsVolume<Cache::Strategy::NoCache> volume{
      std::wstring_view(file.wstring())};

  CHECK_FALSE(volume.IsVolumeOK());
}

#endif  // _WIN32

TEST_CASE("the file log target opens a path of more than 260 units",
          "[io][long-path][logging]") {
  const LongTempDir directory(300);
  const Fs::path log = directory.FileIn("ntfs.log");
  REQUIRE(log.native().size() > 260);

  NTFS_BROWSER_NAMESPACE::Log::Config config;
  config.console_level = NTFS_BROWSER_NAMESPACE::Log::Level::Off;
  config.file_level = NTFS_BROWSER_NAMESPACE::Log::Level::Trace;
  config.file_path = log;
  REQUIRE(NTFS_BROWSER_NAMESPACE::Log::Configure(config));
  NTFS_BROWSER_NAMESPACE::Log::Error("long-path-line");
  NTFS_BROWSER_NAMESPACE::Tests::InstallCaptureSink();

  std::ifstream input(NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(log));
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  CHECK_THAT(text, ContainsSubstring("long-path-line"));
}

TEST_CASE("the file log target makes the folders of a long path",
          "[io][long-path][logging]") {
  const LongTempDir directory(300);
  const Fs::path log =
      directory.Directory() / "new-folder" / "inner" / "ntfs.log";

  NTFS_BROWSER_NAMESPACE::Log::Config config;
  config.console_level = NTFS_BROWSER_NAMESPACE::Log::Level::Off;
  config.file_level = NTFS_BROWSER_NAMESPACE::Log::Level::Trace;
  config.file_path = log;
  REQUIRE(NTFS_BROWSER_NAMESPACE::Log::Configure(config));
  NTFS_BROWSER_NAMESPACE::Log::Error("made-folders-line");
  NTFS_BROWSER_NAMESPACE::Tests::InstallCaptureSink();

  std::ifstream input(NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(log));
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  CHECK_THAT(text, ContainsSubstring("made-folders-line"));
}
