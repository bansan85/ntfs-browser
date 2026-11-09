#ifdef _WIN32

  #include <cstddef>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <memory>
  #include <string>
  #include <vector>

  #include <catch2/catch_template_test_macros.hpp>
  #include <catch2/catch_test_macros.hpp>
  #include <catch2/generators/catch_generators.hpp>

  #include <ntfs-browser/namespace.h>
  #include <ntfs-browser/test/device-access/temp-dir.h>
  #include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
  #include <ntfs-browser/test/support/host-media-support.h>

  #include "attr/mask.h"
  #include "bench/bench.h"
  #include "cache/strategy.h"
  #include "directory-read-ahead-support.h"
  #include "disk-reader.h"
  #include "io/directory-read-ahead.h"
  #include "io/file-record.h"
  #include "io/host-media-kind.h"
  #include "io/win32-disk-reader.h"
  #include "mft/idx.h"
  #include "ntfs-volume.h"
  #include "tuning/tuning.h"
  #include "volume-options.h"
  #include "win-types.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace TestIo = NTFS_BROWSER_NAMESPACE::Test::Io;
namespace TestDeviceAccess = NTFS_BROWSER_NAMESPACE::Test::DeviceAccess;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tuning::MediaKind;

namespace {

// The tree the lane is planned on: sibling directories of 6 files each. The
// directories that a test plans are 10 apart, as in the lane tests.
constexpr std::size_t directories = 44;
constexpr std::size_t files_per_directory = 6;
constexpr std::size_t planned_every = 10;
constexpr std::size_t planned = 4;
constexpr std::uint64_t read_ahead_budget = std::uint64_t{16} << 20U;

// An image file under the temp folder.
class ImageFile {
 public:
  explicit ImageFile(const std::vector<BYTE>& image) {
    path_ = dir_.Path() / "image.img";
    std::ofstream out(path_, std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
  }

  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

 private:
  TestDeviceAccess::TempDir dir_;
  std::filesystem::path path_;
};

// The runs that the read ahead plans for the first directories of the tree,
// on a volume opened from the image file.
template <Cache::Strategy S>
std::uint64_t RunsPlannedOn(const ImageFile& file,
                            const VolumeOptions& options) {
  NtfsVolume<S> volume(file.Path().wstring(), options);
  REQUIRE(volume.IsVolumeOK());
  Io::FileRecord<S> listing(volume);
  listing.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(listing.ParseFileRecord(
      static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::Root)));
  REQUIRE(listing.ParseAttrs());

  Io::DirectoryReadAhead<S> ahead(volume);
  ahead.Attach(listing);
  ahead.Begin();
  std::vector<ULONGLONG> upcoming;
  for (std::size_t index = 0; index < planned; index++) {
    upcoming.push_back(TestFakeImage::name_tree_first_record +
                       (index * planned_every * (files_per_directory + 1)));
  }
  ahead.Plan(upcoming);
  return ahead.GetStats().runs_planned;
}

VolumeOptions OptionsWithLane() {
  VolumeOptions options;
  options.tuning.walk.read_ahead_dirs = 8;
  options.tuning.walk.read_ahead_bytes = read_ahead_budget;
  options.tuning.walk.read_ahead_runs = 64;
  return options;
}

}  // namespace

TEST_CASE("A Win32DiskReader over an image file takes the kind of its host",
          "[io][media-kind][win32]") {
  const ImageFile file(TestFakeImage::BuildFakeNtfsImage());
  const MediaKind kind = GENERATE(MediaKind::Unknown, MediaKind::SolidState,
                                  MediaKind::Rotational);
  const TestSupport::HostMediaAnswer answer(kind);

  Io::Win32DiskReader reader;
  REQUIRE(reader.Open(file.Path().wstring()));

  CHECK(reader.GetMediaKind() == kind);
  // Asked once, in Open(), and about the extended path of the file.
  REQUIRE(TestSupport::HostMediaAnswer::Asked().size() == 1);
  CHECK(TestSupport::HostMediaAnswer::Asked().front().ends_with(L"image.img"));
}

TEST_CASE("The reader of a replay asks no host volume",
          "[io][media-kind][win32]") {
  const ImageFile file(TestFakeImage::BuildFakeNtfsImage());
  const TestSupport::HostMediaAnswer answer(MediaKind::Rotational);

  const std::unique_ptr<IDiskReader> reader =
      NTFS_BROWSER_NAMESPACE::Bench::OpenPlatformReader(file.Path().wstring(),
                                                        {});
  REQUIRE(reader != nullptr);

  CHECK(reader->GetMediaKind() == MediaKind::Unknown);
  CHECK(TestSupport::HostMediaAnswer::Asked().empty());
}

TEST_CASE("A path that does not open asks no host volume",
          "[io][media-kind][win32]") {
  const TestSupport::HostMediaAnswer answer(MediaKind::Rotational);
  const TestDeviceAccess::TempDir dir;

  const std::unique_ptr<IDiskReader> reader =
      NTFS_BROWSER_NAMESPACE::Bench::OpenPlatformReader(
          (dir.Path() / "missing.img").wstring(), {});

  CHECK(reader == nullptr);
  CHECK(TestSupport::HostMediaAnswer::Asked().empty());
}

TEST_CASE("Without a test answer, the query of the tests says Unknown",
          "[io][media-kind][win32]") {
  const ImageFile file(TestFakeImage::BuildFakeNtfsImage());

  CHECK(Io::QueryHostMediaKind(file.Path().wstring()) == MediaKind::Unknown);
}

TEMPLATE_TEST_CASE_SIG("A volume on an image file reports the kind of its host",
                       "[io][media-kind][win32]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ImageFile file(TestFakeImage::BuildFakeNtfsImage());
  const MediaKind kind = GENERATE(MediaKind::Unknown, MediaKind::SolidState,
                                  MediaKind::Rotational);
  const TestSupport::HostMediaAnswer answer(kind);

  const NtfsVolume<S> volume(file.Path().wstring());

  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetMediaKind() == kind);
}

TEMPLATE_TEST_CASE_SIG(
    "VolumeOptions::media_kind wins over the host and spares the query",
    "[io][media-kind][win32]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ImageFile file(TestFakeImage::BuildFakeNtfsImage());
  const TestSupport::HostMediaAnswer answer(MediaKind::Rotational);
  VolumeOptions options;
  options.media_kind = MediaKind::SolidState;

  const NtfsVolume<S> volume(file.Path().wstring(), options);

  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetMediaKind() == MediaKind::SolidState);
  CHECK(TestSupport::HostMediaAnswer::Asked().empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An image file on a hard disk plans without the standing lane",
    "[io][media-kind][win32][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ImageFile file(TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::SpacedTree(directories, files_per_directory)));

  SECTION("a solid state host plans the lane") {
    const TestSupport::HostMediaAnswer answer(MediaKind::SolidState);
    CHECK(RunsPlannedOn<S>(file, OptionsWithLane()) > 0);
  }

  SECTION("a host that does not say plans the lane") {
    const TestSupport::HostMediaAnswer answer(MediaKind::Unknown);
    CHECK(RunsPlannedOn<S>(file, OptionsWithLane()) > 0);
  }

  SECTION("a rotational host does not") {
    const TestSupport::HostMediaAnswer answer(MediaKind::Rotational);
    CHECK(RunsPlannedOn<S>(file, OptionsWithLane()) == 0);
  }

  SECTION("a rotational host with a stated solid state kind plans the lane") {
    const TestSupport::HostMediaAnswer answer(MediaKind::Rotational);
    VolumeOptions options = OptionsWithLane();
    options.media_kind = MediaKind::SolidState;
    CHECK(RunsPlannedOn<S>(file, options) > 0);
  }
}

#endif  // _WIN32
