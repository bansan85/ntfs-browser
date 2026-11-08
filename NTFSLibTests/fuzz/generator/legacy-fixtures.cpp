#include "legacy-fixtures.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "bpb-edit.h"
#include "data/ntfs-bpb.h"
#include "legacy-geometry.h"
#include "resource-locator.h"
#include "transient-damage-reader.h"

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
    case Boot::LegacyEarly:
      LegacyGeometry::Apply(image, LegacyGeometry::Era::Early);
      ApplyBoot(image, Boot::UnsetTotalSectors);
      break;
    case Boot::LegacyLate:
      LegacyGeometry::Apply(image, LegacyGeometry::Era::Late);
      ApplyBoot(image, Boot::UnsetTotalSectors);
      break;
    case Boot::PatchedTotalSectors:
      BpbEdit::Apply(image, [](Data::NtfsBpb& bpb) {
        bpb.total_sectors = std::numeric_limits<std::uint64_t>::max();
      });
      break;
  }
}

void LegacyFixtures::RegisterDamaged(ScenarioRegistry& registry,
                                     const std::vector<Damaged>& inputs) {
  for (const Damaged& input : inputs) {
    const auto make_intact = [input] {
      std::vector<BYTE> image = input.base();
      ApplyBoot(image, input.early ? Boot::LegacyEarly : Boot::LegacyLate);
      if (input.prepare) {
        input.prepare(image);
      }
      return image;
    };
    if (input.transient_reads == 0) {
      registry.AddRecorded(
          input.name,
          [input, make_intact] {
            std::vector<BYTE> image = make_intact();
            if (input.damage) {
              input.damage(image);
            }
            return image;
          },
          input.options, input.probe, input.needs);
      continue;
    }
    registry.Add(
        {.name = input.name,
         .needs = input.needs,
         .make = [input, make_intact](const ResourceLocator& /*resources*/) {
           std::vector<BYTE> intact = make_intact();
           std::vector<BYTE> damaged = intact;
           input.damage(damaged);
           const std::size_t size = intact.size();
           return RecordedStream::Record(
               std::make_unique<TransientDamageReader>(std::move(intact),
                                                       std::move(damaged),
                                                       input.transient_reads),
               size, input.options, input.probe);
         }});
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
