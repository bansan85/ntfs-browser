#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <limits>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "boot-sector-support.h"

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

// Image whose total_sectors is the given value.
BootImageMaker WithTotalSectors(ULONGLONG total_sectors) {
  return EditedBpb(
      [total_sectors](auto& bpb) { bpb.total_sectors = total_sectors; });
}

// Image whose $MFT sits at largest_lcn_mft and whose total_sectors is the
// given value.
BootImageMaker WithLargestMft(ULONGLONG total_sectors) {
  return EditedBpb([total_sectors](auto& bpb) {
    bpb.lcn_mft = largest_lcn_mft;
    bpb.total_sectors = total_sectors;
  });
}

// A total_sectors value, named by what it checks.
struct TotalSectorsCase {
  const char* name;
  ULONGLONG total_sectors;
};

const std::array<TotalSectorsCase, 5> accepted_totals{{
    {"a volume that ends exactly with the first $MFT record is accepted",
     sectors_to_first_record_end},
    {"the largest total_sectors does not overflow the byte count",
     std::numeric_limits<ULONGLONG>::max()},
    // 2^55 sectors of 512 bytes is 2^64 bytes, which wraps to 0.
    {"a byte count that wraps to zero is not taken for an empty volume",
     1ULL << 55U},
    // (2^55 + 2) sectors of 512 bytes wraps to 1024 bytes, below the record.
    {"a byte count that wraps to a small value is not taken for it",
     (1ULL << 55U) + 2},
    // The BPB declares far more sectors than the 64 KiB the image holds.
    {"an image truncated below total_sectors is still accepted", 1ULL << 30U},
}};

// A total_sectors value a strict volume rejects as ending before the first
// $MFT record. Sizes and addresses are whole sectors: a sector is the smallest
// step.
const std::array<TotalSectorsCase, 2> rejected_totals{{
    {"a volume that ends one sector short of it is rejected",
     sectors_to_first_record_end - 1},
    {"a volume that ends before the $MFT address is rejected", 1},
}};

// Scenarios on a null total_sectors and on one that ends before the $MFT.
std::vector<BootScenario> NullAndShortScenarios() {
  return {
      {.name = "a valid image is accepted and logs no total_sectors message",
       .image = UntouchedImage(),
       .ok = true,
       .contains = {"Total sectors = 128"},
       .absent = {null_message, beyond_message}},
      {.name = "strict mode rejects a null total_sectors",
       .image = WithTotalSectors(0),
       .ok = false,
       .contains = {null_message},
       .absent = {invalid_address_message}},
      {.name = "recover mode accepts a null total_sectors",
       .image = WithTotalSectors(0),
       .recover = true,
       .ok = true,
       .contains = {null_message},
       .absent = {beyond_message}},
      {.name = "strict mode rejects a first $MFT record past the volume",
       .image = WithTotalSectors(sectors_to_first_record_end - 1),
       .ok = false,
       .contains = {beyond_message},
       .absent = {null_message, invalid_address_message}},
      {.name = "recover mode accepts a first $MFT record past the volume",
       .image = WithTotalSectors(sectors_to_first_record_end - 1),
       .recover = true,
       .ok = true,
       .contains = {beyond_message}},
      {.name = "the checks run last, after the MFT address",
       .image = WithTotalSectors(0),
       .recover = true,
       .in_order = {mft_address_message, total_sectors_message, null_message}},
      {.name = "a rejected volume keeps the decoded geometry",
       .image = WithTotalSectors(0),
       .ok = false,
       .geometry = FakeGeometry()},
  };
}

// Scenarios on the largest addresses and sizes the check can meet.
std::vector<BootScenario> LargestMftScenarios() {
  return {
      {.name = "the largest plausible MFT address does not overflow the sum: "
               "accepted",
       .image = WithLargestMft(std::numeric_limits<ULONGLONG>::max()),
       .contains = {"MFT address = 0x3FFFFFFFFFFFFC00"},
       .absent = {beyond_message}},
      {.name = "the largest plausible MFT address does not overflow the sum: "
               "rejected",
       .image = WithLargestMft(2),
       .ok = false,
       .contains = {beyond_message}},
      {.name = "the largest MFT record is checked to the sector: short by one",
       .image = WithLargestMft(largest_record_end_sectors - 1),
       .contains = {beyond_message}},
      {.name = "the largest MFT record is checked to the sector: exact",
       .image = WithLargestMft(largest_record_end_sectors),
       .absent = {beyond_message}},
      {.name = "an invalid MFT address is still reported by its own message",
       .image = EditedBpb([](auto& bpb) {
         bpb.lcn_mft = std::numeric_limits<ULONGLONG>::max();
         bpb.total_sectors = 0;
       }),
       .ok = false,
       .contains = {invalid_address_message},
       .absent = {null_message, total_sectors_message}},
  };
}

std::vector<BootScenario> TotalSectorsScenarios() {
  std::vector<BootScenario> scenarios = NullAndShortScenarios();
  for (const BootScenario& scenario : LargestMftScenarios()) {
    scenarios.push_back(scenario);
  }
  for (const TotalSectorsCase& total : accepted_totals) {
    scenarios.push_back({.name = total.name,
                         .image = WithTotalSectors(total.total_sectors),
                         .ok = true,
                         .absent = {beyond_message}});
  }
  for (const TotalSectorsCase& total : rejected_totals) {
    scenarios.push_back({.name = total.name,
                         .image = WithTotalSectors(total.total_sectors),
                         .ok = false,
                         .contains = {beyond_message}});
  }
  return scenarios;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks total_sectors",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
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

  RunBootScenarios<S>(TotalSectorsScenarios());
}
