#include <ntfs-browser/win-types.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "boot-sector-support.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
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

// Image whose bytes_per_sector is the given value.
BootImageMaker WithSectorSize(WORD sector_size) {
  return EditedBpb(
      [sector_size](auto& bpb) { bpb.bytes_per_sector = sector_size; });
}

// Image whose sectors_per_cluster byte is the given value.
BootImageMaker WithSectorsPerCluster(BYTE sectors_per_cluster) {
  return EditedBpb([sectors_per_cluster](auto& bpb) {
    bpb.sectors_per_cluster = sectors_per_cluster;
  });
}

// Name of a scenario that tries one value of a field.
std::string NamedFor(const std::string& group, const char* field, int value) {
  return group + ": " + field + " = " + std::to_string(value);
}

// Every size is rejected with the expected message and not the other one.
void AddSectorSizesRejected(std::vector<BootScenario>& scenarios,
                            const std::string& group,
                            std::initializer_list<WORD> sector_sizes,
                            const char* expected, const char* other) {
  for (const WORD sector_size : sector_sizes) {
    scenarios.push_back(
        {.name = NamedFor(group, "bytes_per_sector", sector_size),
         .image = WithSectorSize(sector_size),
         .ok = false,
         .contains = {expected},
         .absent = {other}});
  }
}

// A rejected sector size stops the decoding, strict or recovering.
void AddSectorRejectionIsHard(std::vector<BootScenario>& scenarios) {
  const std::string chain =
      "a rejection stops the chain before the cluster size";
  const std::string hard =
      "a rejection is hard: recover_errors does not salvage it";
  for (const WORD sector_size : {128, 384, 8192}) {
    const std::string hard_name =
        NamedFor(hard, "bytes_per_sector", sector_size);
    scenarios.push_back(
        {.name = NamedFor(chain, "bytes_per_sector", sector_size),
         .image = WithSectorSize(sector_size),
         .contains = {"Sector Size = "},
         .absent = {cluster_log, file_record_log, "Total sectors = "}});
    scenarios.push_back({.name = hard_name + " (strict)",
                         .image = WithSectorSize(sector_size),
                         .ok = false});
    scenarios.push_back({.name = hard_name + " (recover)",
                         .image = WithSectorSize(sector_size),
                         .recover = true,
                         .ok = false,
                         .absent = {cluster_log}});
  }
}

std::vector<BootScenario> SectorSizeScenarios() {
  std::vector<BootScenario> scenarios{
      {.name = "the fake image's sector size is accepted",
       .image = UntouchedImage(),
       .ok = true,
       .absent = {sector_range_message, sector_power_message}},
      {.name = "a bad sector size is reported before a bad cluster size",
       .image = EditedBpb([](auto& bpb) {
         bpb.bytes_per_sector = 384;
         bpb.sectors_per_cluster = 3;
       }),
       .contains = {sector_power_message},
       .absent = {cluster_power_message}},
  };
  for (const WORD sector_size : {256, 512, 1024, 2048, 4096}) {
    scenarios.push_back(
        {.name = NamedFor("every power of two from 256 to 4096 bytes is "
                          "accepted",
                          "bytes_per_sector", sector_size),
         .image = WithSectorSize(sector_size),
         .contains = {cluster_log},
         .absent = {sector_range_message, sector_power_message}});
  }
  AddSectorSizesRejected(scenarios, "a size below 256 bytes is out of range",
                         {0, 1, 2, 128, 255}, sector_range_message,
                         sector_power_message);
  AddSectorSizesRejected(scenarios, "a size above 4096 bytes is out of range",
                         {4097, 8192, 65535}, sector_range_message,
                         sector_power_message);
  AddSectorSizesRejected(
      scenarios, "a size in range that is not a power of two is rejected",
      {257, 384, 511, 513, 3000, 4095}, sector_power_message,
      sector_range_message);
  AddSectorRejectionIsHard(scenarios);
  return scenarios;
}

// The sectors_per_cluster values that decode to a power-of-two cluster.
void AddClusterValuesAccepted(std::vector<BootScenario>& scenarios) {
  for (const BYTE sectors_per_cluster : {1, 2, 4, 8, 16, 32, 64, 128}) {
    scenarios.push_back(
        {.name = NamedFor("a positive power of two is accepted",
                          "sectors_per_cluster", sectors_per_cluster),
         .image = WithSectorsPerCluster(sectors_per_cluster),
         .contains = {file_record_log},
         .absent = {cluster_power_message}});
  }
  for (int shift = 1; shift <= 12; ++shift) {
    scenarios.push_back(
        {.name = NamedFor("the negative encoding is a power of two by "
                          "construction",
                          "sectors_per_cluster", -shift),
         .image = WithSectorsPerCluster(static_cast<BYTE>(-shift)),
         .contains = {file_record_log},
         .absent = {cluster_power_message, cluster_magnitude_message}});
  }
  for (const WORD sector_size : {256, 512, 1024, 2048, 4096}) {
    scenarios.push_back(
        {.name = NamedFor("every valid sector size keeps a power-of-two "
                          "cluster",
                          "bytes_per_sector", sector_size),
         .image = EditedBpb([sector_size](auto& bpb) {
           bpb.bytes_per_sector = sector_size;
           bpb.sectors_per_cluster = 64;
         }),
         .contains = {file_record_log},
         .absent = {cluster_power_message}});
  }
}

// The sectors_per_cluster values that stop the decoding at the cluster size.
void AddClusterValuesRejected(std::vector<BootScenario>& scenarios) {
  const std::string chain =
      "a rejection stops the chain before the file record size";
  const std::string hard =
      "a rejection is hard: recover_errors does not salvage it";
  for (const BYTE sectors_per_cluster : {3, 5, 6, 7, 9, 100, 127}) {
    scenarios.push_back(
        {.name = NamedFor("a positive value that is not a power of two is "
                          "rejected",
                          "sectors_per_cluster", sectors_per_cluster),
         .image = WithSectorsPerCluster(sectors_per_cluster),
         .ok = false,
         .contains = {cluster_log, cluster_power_message}});
  }
  for (const BYTE sectors_per_cluster : {3, 127}) {
    scenarios.push_back(
        {.name = NamedFor(chain, "sectors_per_cluster", sectors_per_cluster),
         .image = WithSectorsPerCluster(sectors_per_cluster),
         .absent = {file_record_log, "IndexBlock Size = ", "MFT address = "}});
  }
  for (const BYTE sectors_per_cluster : {3, 5, 6, 127}) {
    const std::string hard_name =
        NamedFor(hard, "sectors_per_cluster", sectors_per_cluster);
    scenarios.push_back({.name = hard_name + " (strict)",
                         .image = WithSectorsPerCluster(sectors_per_cluster),
                         .ok = false});
    scenarios.push_back({.name = hard_name + " (recover)",
                         .image = WithSectorsPerCluster(sectors_per_cluster),
                         .recover = true,
                         .ok = false,
                         .contains = {cluster_power_message},
                         .absent = {file_record_log}});
  }
}

std::vector<BootScenario> SectorsPerClusterScenarios() {
  std::vector<BootScenario> scenarios{
      {.name = "0x80 counts 128 sectors, not a shift of -128",
       .image = WithSectorsPerCluster(0x80),
       .contains = {"Cluster Size = 65536 bytes", file_record_log},
       .absent = {cluster_magnitude_message}},
      {.name = "a null value keeps its own message",
       .image = WithSectorsPerCluster(0),
       .ok = false,
       .contains = {cluster_null_message},
       .absent = {cluster_power_message}},
      {.name = "a null value is rejected under recover_errors too",
       .image = WithSectorsPerCluster(0),
       .recover = true,
       .ok = false,
       .contains = {cluster_null_message}},
      {.name = "a negative magnitude beyond 12 keeps its own message",
       .image = WithSectorsPerCluster(static_cast<BYTE>(-13)),
       .ok = false,
       .contains = {cluster_magnitude_message},
       .absent = {cluster_power_message}},
  };
  AddClusterValuesAccepted(scenarios);
  AddClusterValuesRejected(scenarios);
  return scenarios;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks the sector size",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunBootScenarios<S>(SectorSizeScenarios());

  SECTION("a rejected volume leaves the later geometry at zero") {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        ImageWithBpb([](auto& bpb) { bpb.bytes_per_sector = 384; })));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetClusterSize() == 0);
    CHECK(volume.GetFileRecordSize() == 0);
    CHECK(volume.GetIndexBlockSize() == 0);
    CHECK(volume.GetMFTAddr() == 0);
    CHECK(volume.GetClusterBuffer().empty());
  }
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks sectors_per_cluster",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunBootScenarios<S>(SectorsPerClusterScenarios());
}
