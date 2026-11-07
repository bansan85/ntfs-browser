#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/bpb-edit.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void LegacyFixtures::ApplyBoot(std::vector<BYTE>& image, Boot boot) {
  switch (boot) {
    case Boot::Current:
      break;
    case Boot::UnsetTotalSectors:
      BpbEdit::Apply(image, [](Data::NtfsBpb& bpb) {
        bpb.total_sectors = 0;
        std::swap(bpb.x_aa, bpb.x_55);
      });
      break;
    case Boot::PatchedTotalSectors:
      BpbEdit::Apply(image, [](Data::NtfsBpb& bpb) {
        bpb.total_sectors = std::numeric_limits<std::uint64_t>::max();
      });
      break;
  }
}

void LegacyFixtures::Register(ScenarioRegistry& registry,
                              const std::vector<Fixture>& fixtures) {
  for (const Fixture& fixture : fixtures) {
    registry.AddRecorded(
        fixture.name,
        [make = fixture.make, boot = fixture.boot] {
          std::vector<BYTE> image = make();
          ApplyBoot(image, boot);
          return image;
        },
        fixture.options, fixture.probe, fixture.needs);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
