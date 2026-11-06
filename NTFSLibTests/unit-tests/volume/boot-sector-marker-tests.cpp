#include <ntfs-browser/win-types.h>

#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>

#include "boot-sector-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// Message logged when the boot sector does not end with 0xAA55.
constexpr const char* marker_message = "Boot sector end marker is not 0xAA55";

// First message of the geometry log, which the marker check MUST precede.
constexpr const char* geometry_message = "Sector Size = ";

// Message logged when the file system name is not NTFS.
constexpr const char* not_ntfs_message = "Volume file system is not NTFS";

// Both marker bytes cleared.
BootImageMaker NoMarker() {
  return EditedBpb([](auto& bpb) {
    bpb.x_aa = 0;
    bpb.x_55 = 0;
  });
}

std::vector<BootScenario> MarkerScenarios() {
  return {
      {.name = "a valid marker is accepted and logs no marker message",
       .image = UntouchedImage(),
       .ok = true,
       .contains = {geometry_message},
       .absent = {marker_message}},
      {.name = "strict mode rejects a missing marker before decoding the "
               "geometry",
       .image = NoMarker(),
       .ok = false,
       .geometry = BootGeometry{},
       .contains = {marker_message},
       .absent = {geometry_message}},
      {.name = "strict mode rejects a wrong first marker byte",
       .image = EditedBpb([](auto& bpb) { bpb.x_aa = 0xAA; }),
       .ok = false},
      {.name = "strict mode rejects a wrong second marker byte",
       .image = EditedBpb([](auto& bpb) { bpb.x_55 = 0x55; }),
       .ok = false},
      {.name = "recover mode accepts a missing marker and decodes the geometry",
       .image = NoMarker(),
       .recover = true,
       .ok = true,
       .geometry = FakeGeometry(),
       .contains = {marker_message, geometry_message},
       .in_order = {marker_message, geometry_message}},
      {.name = "recover mode keeps a valid marker silent",
       .image = UntouchedImage(),
       .recover = true,
       .ok = true,
       .absent = {marker_message}},
      {.name = "a bad signature is still reported first",
       .image = EditedBpb([](auto& bpb) {
         bpb.signature[0] = 'X';
         bpb.x_aa = 0;
       }),
       .recover = true,
       .ok = false,
       .contains = {not_ntfs_message},
       .absent = {marker_message}},
  };
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks the boot sector 0xAA55 marker",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunBootScenarios<S>(MarkerScenarios());
}
