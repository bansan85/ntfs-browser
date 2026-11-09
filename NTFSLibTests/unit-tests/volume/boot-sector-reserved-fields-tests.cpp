#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "boot-sector-support.h"
#include "cache/strategy.h"
#include "win-types.h"

using Bpb = NTFS_BROWSER_NAMESPACE::Data::NtfsBpb;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// Prefix of the message logged when a field that must be zero is not.
constexpr const char* reserved_message =
    "Boot sector fields that must be zero are not: ";

// First message of the geometry log, which the check MUST precede.
constexpr const char* geometry_message = "Sector Size = ";

// Message of the end marker check, which runs before the reserved fields.
constexpr const char* marker_message = "Boot sector end marker is not 0xAA55";

// A BPB region that a real NTFS volume keeps at zero.
struct ZeroRegion {
  // Name the message gives for the region.
  std::string_view name;
  // Offset of the region in the boot sector.
  size_t offset;
  // Size of the region in bytes.
  size_t size;
};

// The regions the check enforces, at the offsets the NTFS layout gives.
constexpr std::array<ZeroRegion, 5> zero_regions{{
    {"reserved_sectors (0x0E)", 0x0E, 2},
    {"fat_count_and_root_entries (0x10)", 0x10, 3},
    {"small_sectors (0x13)", 0x13, 2},
    {"sectors_per_fat (0x16)", 0x16, 2},
    {"large_sectors (0x20)", 0x20, 4},
}};

static_assert(offsetof(Bpb, reserved_sectors) == 0x0E);
static_assert(offsetof(Bpb, zeros1) == 0x10);
static_assert(offsetof(Bpb, not_used1) == 0x13);
static_assert(offsetof(Bpb, zeros2) == 0x16);
static_assert(offsetof(Bpb, not_used2) == 0x20);
static_assert(offsetof(Bpb, not_used3) == 0x24);

// Message of the signature check, which runs before everything else.
constexpr const char* not_ntfs_message = "Volume file system is not NTFS";

// Message of the sector size check, which stays a hard error.
constexpr const char* sector_size_message = "Sector Size must be";

// Adds the scenarios of one region, strict and recovering.
void AddRegionScenarios(std::vector<BootScenario>& scenarios,
                        const ZeroRegion& region) {
  const std::string name(region.name);
  scenarios.push_back({.name = "strict mode rejects each region before "
                               "decoding the geometry: " +
                               name,
                       .image = ByteSetAt(region.offset),
                       .ok = false,
                       .geometry = BootGeometry{},
                       .contains = {reserved_message, name},
                       .absent = {geometry_message}});
  scenarios.push_back({.name = "recover mode accepts each region and decodes "
                               "the geometry: " +
                               name,
                       .image = ByteSetAt(region.offset),
                       .recover = true,
                       .ok = true,
                       .geometry = FakeGeometry(),
                       .contains = {reserved_message, name, geometry_message},
                       .in_order = {reserved_message, geometry_message}});
  for (size_t byte = 0; byte < region.size; ++byte) {
    scenarios.push_back(
        {.name = "strict mode rejects each single byte of each region: " +
                 name + " byte " + std::to_string(byte),
         .image = ByteSetAt(region.offset + byte),
         .ok = false,
         .contains = {name}});
  }
}

// Adds a scenario for each BPB field the check leaves alone.
void AddUncheckedScenarios(std::vector<BootScenario>& scenarios) {
  constexpr std::array<size_t, 5> unchecked{
      offsetof(Bpb, media_descriptor), offsetof(Bpb, sectors_per_track),
      offsetof(Bpb, number_of_heads),  offsetof(Bpb, hidden_sectors),
      offsetof(Bpb, not_used3),
  };
  for (const size_t offset : unchecked) {
    scenarios.push_back(
        {.name = "each unchecked field is accepted on its own: offset " +
                 std::to_string(offset),
         .image = ByteSetAt(offset),
         .ok = true,
         .absent = {reserved_message}});
  }
}

// Scenarios that open one edited image.
void AddEditedScenarios(std::vector<BootScenario>& scenarios) {
  scenarios.push_back(
      {.name = "a one-bit value is enough to reject",
       .image = EditedBpb([](Bpb& bpb) { bpb.reserved_sectors = 1; }),
       .ok = false,
       .contains = {reserved_message}});
  scenarios.push_back(
      {.name = "one message names every offending region",
       .image = EditedBpb([](Bpb& bpb) {
         bpb.reserved_sectors = 1;
         bpb.not_used1 = 1;
         bpb.not_used2 = 1;
       }),
       .ok = false,
       .contains = {"reserved_sectors (0x0E)", "small_sectors (0x13)",
                    "large_sectors (0x20)"},
       .absent = {"sectors_per_fat", "fat_count_and_root_entries"},
       .once = reserved_message});
  scenarios.push_back(
      {.name = "recover mode logs the message once for "
               "several regions",
       .image = EditedBpb([](Bpb& bpb) {
         bpb.zeros2 = 0xFFFF;
         bpb.not_used2 = 0xFFFFFFFF;
       }),
       .recover = true,
       .ok = true,
       .contains = {"sectors_per_fat (0x16)", "large_sectors (0x20)"}});
  scenarios.push_back(
      {.name = "fields real volumes fill in are accepted silently",
       .image = EditedBpb([](Bpb& bpb) {
         bpb.media_descriptor = 0xF8;
         bpb.sectors_per_track = 63;
         bpb.number_of_heads = 255;
         bpb.hidden_sectors = 2048;
         bpb.not_used3 = 0x80008000;
       }),
       .ok = true,
       .absent = {reserved_message}});
}

// Scenarios on the order of the boot sector checks.
void AddOrderScenarios(std::vector<BootScenario>& scenarios) {
  const auto marker_and_reserved = EditedBpb([](Bpb& bpb) {
    bpb.x_aa = 0;
    bpb.reserved_sectors = 1;
  });
  scenarios.push_back(
      {.name = "a missing marker is reported before the reserved fields",
       .image = marker_and_reserved,
       .ok = false,
       .contains = {marker_message},
       .absent = {reserved_message}});
  scenarios.push_back(
      {.name = "recover mode reports the marker, then the reserved fields",
       .image = marker_and_reserved,
       .recover = true,
       .ok = true,
       .in_order = {marker_message, reserved_message, geometry_message}});
  scenarios.push_back({.name = "a bad signature is still reported first",
                       .image = EditedBpb([](Bpb& bpb) {
                         bpb.signature[0] = 'X';
                         bpb.reserved_sectors = 1;
                       }),
                       .recover = true,
                       .ok = false,
                       .contains = {not_ntfs_message},
                       .absent = {reserved_message}});
  scenarios.push_back(
      {.name = "a bad sector size is still rejected in recover mode",
       .image = EditedBpb([](Bpb& bpb) {
         bpb.bytes_per_sector = 3;
         bpb.reserved_sectors = 1;
       }),
       .recover = true,
       .ok = false,
       .contains = {reserved_message, sector_size_message}});
}

std::vector<BootScenario> ReservedFieldsScenarios() {
  std::vector<BootScenario> scenarios{
      {.name = "a valid image is accepted and logs no reserved fields message",
       .image = UntouchedImage(),
       .ok = true,
       .contains = {geometry_message},
       .absent = {reserved_message}},
      {.name = "recover mode keeps a valid image silent",
       .image = UntouchedImage(),
       .recover = true,
       .ok = true,
       .absent = {reserved_message}},
  };
  for (const ZeroRegion& region : zero_regions) {
    AddRegionScenarios(scenarios, region);
  }
  AddUncheckedScenarios(scenarios);
  AddEditedScenarios(scenarios);
  AddOrderScenarios(scenarios);
  return scenarios;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks the BPB reserved fields are zero",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunBootScenarios<S>(ReservedFieldsScenarios());
}
