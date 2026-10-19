#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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

// Message logged when total_sectors is null.
constexpr const char* null_message = "Total sectors can't be null";

// Message logged when the first $MFT record does not fit in the volume.
constexpr const char* beyond_message =
    "First $MFT record lies beyond the volume";

// Message of the existing MFT address check, which keeps its own meaning.
constexpr const char* invalid_address_message = "MFT address is invalid";

// Logged just before the total_sectors checks.
constexpr const char* mft_address_message = "MFT address = ";

// Logged by the total_sectors checks, whatever they conclude.
constexpr const char* total_sectors_message = "Total sectors = ";

// The fake image's $MFT address: one cluster in.
constexpr ULONGLONG fake_mft_addr =
    NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size;

// Sectors from the start of the volume to the end of the fake image's first
// $MFT record.
constexpr ULONGLONG sectors_to_first_record_end =
    (fake_mft_addr + NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size) /
    NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector;

// lcn_mft of the largest plausible MFT address: (2^52 - 1) clusters of 1024
// bytes, the last address under 2^62.
constexpr ULONGLONG largest_lcn_mft = (1ULL << 52U) - 1;

// The address largest_lcn_mft gives, in bytes.
constexpr ULONGLONG largest_mft_addr = 0x3FFFFFFFFFFFFC00ULL;

// Sectors to the end of the first record at largest_mft_addr.
constexpr ULONGLONG largest_record_end_sectors =
    (largest_mft_addr + NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size) /
    NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector;

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  return image;
}

// Image whose total_sectors is the given value.
std::vector<BYTE> ImageWithTotalSectors(ULONGLONG total_sectors) {
  return ImageWithBpb(
      [total_sectors](auto& bpb) { bpb.total_sectors = total_sectors; });
}

// Image whose $MFT sits at largest_lcn_mft and whose total_sectors is the
// given value.
std::vector<BYTE> ImageWithLargestMft(ULONGLONG total_sectors) {
  return ImageWithBpb([total_sectors](auto& bpb) {
    bpb.lcn_mft = largest_lcn_mft;
    bpb.total_sectors = total_sectors;
  });
}

// What opening an image logged, and whether the volume came up.
struct Opened {
  bool ok{false};
  std::string log;
};

template <Cache::Strategy S>
Opened Open(std::vector<BYTE> image, VolumeOptions options = {}) {
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)), options);
  return {.ok = volume.IsVolumeOK(),
          .log = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog()};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks total_sectors",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const VolumeOptions recover{.recover_errors = true};

  SECTION("the fake image declares the sectors it holds") {
    const std::vector<BYTE> image =
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
    const auto& bpb =
        *reinterpret_cast<const NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(
            image.data());

    CHECK(bpb.total_sectors *
              NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector ==
          image.size());
  }

  SECTION("a valid image is accepted and logs no total_sectors message") {
    const Opened opened =
        Open<S>(NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage());

    CHECK(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring("Total sectors = 128"));
    CHECK_THAT(opened.log, !ContainsSubstring(null_message));
    CHECK_THAT(opened.log, !ContainsSubstring(beyond_message));
  }

  SECTION("strict mode rejects a null total_sectors") {
    const Opened opened = Open<S>(ImageWithTotalSectors(0));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(null_message));
    CHECK_THAT(opened.log, !ContainsSubstring(invalid_address_message));
  }

  SECTION("recover mode accepts a null total_sectors") {
    const Opened opened = Open<S>(ImageWithTotalSectors(0), recover);

    CHECK(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(null_message));
    // A null count already says everything: no second message.
    CHECK_THAT(opened.log, !ContainsSubstring(beyond_message));
  }

  SECTION("strict mode rejects a first $MFT record past the volume") {
    const Opened opened =
        Open<S>(ImageWithTotalSectors(sectors_to_first_record_end - 1));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(beyond_message));
    CHECK_THAT(opened.log, !ContainsSubstring(null_message));
    CHECK_THAT(opened.log, !ContainsSubstring(invalid_address_message));
  }

  SECTION("recover mode accepts a first $MFT record past the volume") {
    const Opened opened = Open<S>(
        ImageWithTotalSectors(sectors_to_first_record_end - 1), recover);

    CHECK(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(beyond_message));
  }

  SECTION("a volume that ends exactly with the first $MFT record is accepted") {
    const Opened opened =
        Open<S>(ImageWithTotalSectors(sectors_to_first_record_end));

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(beyond_message));
  }

  SECTION("a volume that ends one sector short of it is rejected") {
    // Sizes and addresses are whole sectors: a sector is the smallest step.
    const Opened strict =
        Open<S>(ImageWithTotalSectors(sectors_to_first_record_end - 1));

    CHECK_FALSE(strict.ok);
    CHECK_THAT(strict.log, ContainsSubstring(beyond_message));
  }

  SECTION("a volume that ends before the $MFT address is rejected") {
    const Opened opened = Open<S>(ImageWithTotalSectors(1));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(beyond_message));
  }

  SECTION("the largest total_sectors does not overflow the byte count") {
    const Opened opened =
        Open<S>(ImageWithTotalSectors(std::numeric_limits<ULONGLONG>::max()));

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(beyond_message));
  }

  SECTION("a byte count that wraps to zero is not taken for an empty volume") {
    // 2^55 sectors of 512 bytes is 2^64 bytes, which wraps to 0.
    const Opened opened = Open<S>(ImageWithTotalSectors(1ULL << 55U));

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(beyond_message));
  }

  SECTION("a byte count that wraps to a small value is not taken for it") {
    // (2^55 + 2) sectors of 512 bytes wraps to 1024 bytes, below the record.
    const Opened opened = Open<S>(ImageWithTotalSectors((1ULL << 55U) + 2));

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(beyond_message));
  }

  SECTION("the largest plausible MFT address does not overflow the sum") {
    const Opened accepted =
        Open<S>(ImageWithLargestMft(std::numeric_limits<ULONGLONG>::max()));
    const Opened rejected = Open<S>(ImageWithLargestMft(2));

    CHECK_THAT(accepted.log,
               ContainsSubstring("MFT address = 0x3FFFFFFFFFFFFC00"));
    CHECK_THAT(accepted.log, !ContainsSubstring(beyond_message));
    CHECK_THAT(rejected.log, ContainsSubstring(beyond_message));
    CHECK_FALSE(rejected.ok);
  }

  SECTION("the largest MFT record is checked to the sector") {
    const Opened short_by_one =
        Open<S>(ImageWithLargestMft(largest_record_end_sectors - 1));
    const Opened exact =
        Open<S>(ImageWithLargestMft(largest_record_end_sectors));

    CHECK_THAT(short_by_one.log, ContainsSubstring(beyond_message));
    CHECK_THAT(exact.log, !ContainsSubstring(beyond_message));
  }

  SECTION("an image truncated below total_sectors is still accepted") {
    // The BPB declares far more sectors than the 64 KiB the image holds.
    const Opened opened = Open<S>(ImageWithTotalSectors(1ULL << 30U));

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(beyond_message));
  }

  SECTION("an invalid MFT address is still reported by its own message") {
    const Opened opened = Open<S>(ImageWithBpb([](auto& bpb) {
      bpb.lcn_mft = std::numeric_limits<ULONGLONG>::max();
      bpb.total_sectors = 0;
    }));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(invalid_address_message));
    CHECK_THAT(opened.log, !ContainsSubstring(null_message));
    CHECK_THAT(opened.log, !ContainsSubstring(total_sectors_message));
  }

  SECTION("the checks run last, after the MFT address") {
    const Opened opened = Open<S>(ImageWithTotalSectors(0), recover);

    const std::size_t address = opened.log.find(mft_address_message);
    const std::size_t total = opened.log.find(total_sectors_message);
    const std::size_t null_check = opened.log.find(null_message);
    REQUIRE(address != std::string::npos);
    REQUIRE(total != std::string::npos);
    REQUIRE(null_check != std::string::npos);
    CHECK(address < total);
    CHECK(total < null_check);
  }

  SECTION("a rejected volume keeps the decoded geometry") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithTotalSectors(0)));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetSectorSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector);
    CHECK(volume.GetMFTAddr() == fake_mft_addr);
  }
}
