#include <ntfs-browser/win-types.h>

#include <initializer_list>
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

#include "catch2/catch_message.hpp"
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

// Logged when the sector size is outside the supported range.
constexpr const char* sector_range_message =
    "Sector Size must be between 256 and 4096 bytes";

// Logged when the sector size is in range but not a power of two.
constexpr const char* sector_power_message =
    "Sector Size must be a power of two";

// Logged when the cluster size is not a power of two.
constexpr const char* cluster_power_message =
    "Cluster Size must be a power of two";

// Logged for a null cluster size, which keeps its own message.
constexpr const char* cluster_null_message = "Cluster Size can't be null";

// Logged for a sectors_per_cluster magnitude beyond the encoding.
constexpr const char* cluster_magnitude_message =
    "sectors_per_cluster magnitude out of range";

// First line of the cluster check. A sector size rejection MUST stop before
// it.
constexpr const char* cluster_log = "Cluster Size = ";

// First line of the file record check. A cluster size rejection MUST stop
// before it.
constexpr const char* file_record_log = "FileRecord Size = ";

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  return image;
}

// Image whose bytes_per_sector is the given value.
std::vector<BYTE> ImageWithSectorSize(WORD sector_size) {
  return ImageWithBpb(
      [sector_size](auto& bpb) { bpb.bytes_per_sector = sector_size; });
}

// Image whose sectors_per_cluster byte is the given value.
std::vector<BYTE> ImageWithSectorsPerCluster(BYTE sectors_per_cluster) {
  return ImageWithBpb([sectors_per_cluster](auto& bpb) {
    bpb.sectors_per_cluster = sectors_per_cluster;
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

// Every size is rejected with the expected message and not the other one.
template <Cache::Strategy S>
void CheckSectorSizesRejected(std::initializer_list<WORD> sector_sizes,
                              const char* expected, const char* other) {
  for (const WORD sector_size : sector_sizes) {
    INFO("bytes_per_sector = " << sector_size);
    const Opened opened = Open<S>(ImageWithSectorSize(sector_size));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(expected));
    CHECK_THAT(opened.log, !ContainsSubstring(other));
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks the sector size",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const VolumeOptions recover{.recover_errors = true};

  SECTION("the fake image's sector size is accepted") {
    const Opened opened =
        Open<S>(NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage());

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(sector_range_message));
    CHECK_THAT(opened.log, !ContainsSubstring(sector_power_message));
  }

  SECTION("every power of two from 256 to 4096 bytes is accepted") {
    for (const WORD sector_size : {256, 512, 1024, 2048, 4096}) {
      INFO("bytes_per_sector = " << sector_size);
      const Opened opened = Open<S>(ImageWithSectorSize(sector_size));

      CHECK_THAT(opened.log, !ContainsSubstring(sector_range_message));
      CHECK_THAT(opened.log, !ContainsSubstring(sector_power_message));
      // The decoding goes on to the cluster size.
      CHECK_THAT(opened.log, ContainsSubstring(cluster_log));
    }
  }

  SECTION("a size below 256 bytes is out of range") {
    CheckSectorSizesRejected<S>({0, 1, 2, 128, 255}, sector_range_message,
                                sector_power_message);
  }

  SECTION("a size above 4096 bytes is out of range") {
    CheckSectorSizesRejected<S>({4097, 8192, 65535}, sector_range_message,
                                sector_power_message);
  }

  SECTION("a size in range that is not a power of two is rejected") {
    CheckSectorSizesRejected<S>({257, 384, 511, 513, 3000, 4095},
                                sector_power_message, sector_range_message);
  }

  SECTION("a rejection stops the chain before the cluster size") {
    for (const WORD sector_size : {128, 384, 8192}) {
      INFO("bytes_per_sector = " << sector_size);
      const Opened opened = Open<S>(ImageWithSectorSize(sector_size));

      CHECK_THAT(opened.log, ContainsSubstring("Sector Size = "));
      CHECK_THAT(opened.log, !ContainsSubstring(cluster_log));
      CHECK_THAT(opened.log, !ContainsSubstring(file_record_log));
      CHECK_THAT(opened.log, !ContainsSubstring("Total sectors = "));
    }
  }

  SECTION("a rejection is hard: recover_errors does not salvage it") {
    for (const WORD sector_size : {128, 384, 8192}) {
      INFO("bytes_per_sector = " << sector_size);
      const Opened strict = Open<S>(ImageWithSectorSize(sector_size));
      const Opened recovering =
          Open<S>(ImageWithSectorSize(sector_size), recover);

      CHECK_FALSE(strict.ok);
      CHECK_FALSE(recovering.ok);
      CHECK_THAT(recovering.log, !ContainsSubstring(cluster_log));
    }
  }

  SECTION("a rejected volume leaves the later geometry at zero") {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithSectorSize(384)));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetClusterSize() == 0);
    CHECK(volume.GetFileRecordSize() == 0);
    CHECK(volume.GetIndexBlockSize() == 0);
    CHECK(volume.GetMFTAddr() == 0);
    CHECK(volume.GetClusterBuffer().empty());
  }

  SECTION("a bad sector size is reported before a bad cluster size") {
    const Opened opened = Open<S>(ImageWithBpb([](auto& bpb) {
      bpb.bytes_per_sector = 384;
      bpb.sectors_per_cluster = 3;
    }));

    CHECK_THAT(opened.log, ContainsSubstring(sector_power_message));
    CHECK_THAT(opened.log, !ContainsSubstring(cluster_power_message));
  }
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks sectors_per_cluster",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const VolumeOptions recover{.recover_errors = true};

  SECTION("a positive power of two is accepted") {
    for (const BYTE sectors_per_cluster : {1, 2, 4, 8, 16, 32, 64, 128}) {
      INFO("sectors_per_cluster = " << static_cast<int>(sectors_per_cluster));
      const Opened opened =
          Open<S>(ImageWithSectorsPerCluster(sectors_per_cluster));

      CHECK_THAT(opened.log, !ContainsSubstring(cluster_power_message));
      // The decoding goes on to the file record size.
      CHECK_THAT(opened.log, ContainsSubstring(file_record_log));
    }
  }

  SECTION("0x80 counts 128 sectors, not a shift of -128") {
    const Opened opened = Open<S>(ImageWithSectorsPerCluster(0x80));

    CHECK_THAT(opened.log, ContainsSubstring("Cluster Size = 65536 bytes"));
    CHECK_THAT(opened.log, !ContainsSubstring(cluster_magnitude_message));
    CHECK_THAT(opened.log, ContainsSubstring(file_record_log));
  }

  SECTION("a positive value that is not a power of two is rejected") {
    for (const BYTE sectors_per_cluster : {3, 5, 6, 7, 9, 100, 127}) {
      INFO("sectors_per_cluster = " << static_cast<int>(sectors_per_cluster));
      const Opened opened =
          Open<S>(ImageWithSectorsPerCluster(sectors_per_cluster));

      CHECK_FALSE(opened.ok);
      CHECK_THAT(opened.log, ContainsSubstring(cluster_log));
      CHECK_THAT(opened.log, ContainsSubstring(cluster_power_message));
    }
  }

  SECTION("a rejection stops the chain before the file record size") {
    for (const BYTE sectors_per_cluster : {3, 127}) {
      INFO("sectors_per_cluster = " << static_cast<int>(sectors_per_cluster));
      const Opened opened =
          Open<S>(ImageWithSectorsPerCluster(sectors_per_cluster));

      CHECK_THAT(opened.log, !ContainsSubstring(file_record_log));
      CHECK_THAT(opened.log, !ContainsSubstring("IndexBlock Size = "));
      CHECK_THAT(opened.log, !ContainsSubstring("MFT address = "));
    }
  }

  SECTION("a rejection is hard: recover_errors does not salvage it") {
    for (const BYTE sectors_per_cluster : {3, 5, 6, 127}) {
      INFO("sectors_per_cluster = " << static_cast<int>(sectors_per_cluster));
      const Opened strict =
          Open<S>(ImageWithSectorsPerCluster(sectors_per_cluster));
      const Opened recovering =
          Open<S>(ImageWithSectorsPerCluster(sectors_per_cluster), recover);

      CHECK_FALSE(strict.ok);
      CHECK_FALSE(recovering.ok);
      CHECK_THAT(recovering.log, ContainsSubstring(cluster_power_message));
      CHECK_THAT(recovering.log, !ContainsSubstring(file_record_log));
    }
  }

  SECTION("a null value keeps its own message") {
    const Opened opened = Open<S>(ImageWithSectorsPerCluster(0));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(cluster_null_message));
    CHECK_THAT(opened.log, !ContainsSubstring(cluster_power_message));
  }

  SECTION("a null value is rejected under recover_errors too") {
    const Opened opened = Open<S>(ImageWithSectorsPerCluster(0), recover);

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(cluster_null_message));
  }

  SECTION("the negative encoding is a power of two by construction") {
    for (int shift = 1; shift <= 12; ++shift) {
      INFO("sectors_per_cluster = -" << shift);
      const Opened opened =
          Open<S>(ImageWithSectorsPerCluster(static_cast<BYTE>(-shift)));

      CHECK_THAT(opened.log, !ContainsSubstring(cluster_power_message));
      CHECK_THAT(opened.log, !ContainsSubstring(cluster_magnitude_message));
      CHECK_THAT(opened.log, ContainsSubstring(file_record_log));
    }
  }

  SECTION("a negative magnitude beyond 12 keeps its own message") {
    const Opened opened =
        Open<S>(ImageWithSectorsPerCluster(static_cast<BYTE>(-13)));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(cluster_magnitude_message));
    CHECK_THAT(opened.log, !ContainsSubstring(cluster_power_message));
  }

  SECTION("every valid sector size keeps a power-of-two cluster") {
    for (const WORD sector_size : {256, 512, 1024, 2048, 4096}) {
      INFO("bytes_per_sector = " << sector_size);
      const Opened opened = Open<S>(ImageWithBpb([sector_size](auto& bpb) {
        bpb.bytes_per_sector = sector_size;
        bpb.sectors_per_cluster = 64;
      }));

      CHECK_THAT(opened.log, !ContainsSubstring(cluster_power_message));
      CHECK_THAT(opened.log, ContainsSubstring(file_record_log));
    }
  }
}
