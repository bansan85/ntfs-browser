#include <ntfs-browser/win-types.h>

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// Message logged when the boot sector does not end with 0xAA55.
constexpr const char* marker_message = "Boot sector end marker is not 0xAA55";

// First message of the geometry log, which the marker check MUST precede.
constexpr const char* geometry_message = "Sector Size = ";

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  return image;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks the boot sector 0xAA55 marker",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("a valid marker is accepted and logs no marker message") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage()));
    const std::string captured =
        NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

    CHECK(volume.IsVolumeOK());
    CHECK_THAT(captured, !ContainsSubstring(marker_message));
    CHECK_THAT(captured, ContainsSubstring(geometry_message));
  }

  SECTION("strict mode rejects a missing marker before decoding the geometry") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.x_aa = 0;
          bpb.x_55 = 0;
        })));
    const std::string captured =
        NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK_THAT(captured, ContainsSubstring(marker_message));
    CHECK_THAT(captured, !ContainsSubstring(geometry_message));
    CHECK(volume.GetSectorSize() == 0);
    CHECK(volume.GetClusterSize() == 0);
    CHECK(volume.GetClusterBuffer().empty());
  }

  SECTION("strict mode rejects each wrong marker byte") {
    const NtfsVolume<S> wrong_first(
        std::make_unique<MemoryDiskReader>(
            ImageWithBpb([](auto& bpb) { bpb.x_aa = 0xAA; })),
        VolumeOptions{});
    const NtfsVolume<S> wrong_second(
        std::make_unique<MemoryDiskReader>(
            ImageWithBpb([](auto& bpb) { bpb.x_55 = 0x55; })),
        VolumeOptions{});

    CHECK_FALSE(wrong_first.IsVolumeOK());
    CHECK_FALSE(wrong_second.IsVolumeOK());
  }

  SECTION("recover mode accepts a missing marker and decodes the geometry") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.x_aa = 0;
          bpb.x_55 = 0;
        })),
        VolumeOptions{.recover_errors = true});
    const std::string captured =
        NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

    CHECK(volume.IsVolumeOK());
    CHECK(volume.GetSectorSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector);
    CHECK(volume.GetClusterSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
    CHECK_THAT(captured, ContainsSubstring(marker_message));
    CHECK_THAT(captured, ContainsSubstring(geometry_message));
    // The marker is reported first, before any geometry line.
    CHECK(captured.find(marker_message) < captured.find(geometry_message));
  }

  SECTION("recover mode keeps a valid marker silent") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage()),
        VolumeOptions{.recover_errors = true});
    const std::string captured =
        NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

    CHECK(volume.IsVolumeOK());
    CHECK_THAT(captured, !ContainsSubstring(marker_message));
  }

  SECTION("a bad signature is still reported first") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.signature[0] = 'X';
          bpb.x_aa = 0;
        })),
        VolumeOptions{.recover_errors = true});
    const std::string captured =
        NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK_THAT(captured, ContainsSubstring("Volume file system is not NTFS"));
    CHECK_THAT(captured, !ContainsSubstring(marker_message));
  }
}
