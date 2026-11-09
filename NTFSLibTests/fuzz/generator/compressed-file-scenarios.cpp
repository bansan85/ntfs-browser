#include "compressed-file-scenarios.h"

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "legacy-fixtures.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace FakeImage = Test::FakeImage;

void CompressedFileScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry, {
                    {.name = "compressed_data_read",
                     .make = FakeImage::BuildFakeNtfsImageWithCompressedFile,
                     .boot = LegacyFixtures::Boot::PatchedTotalSectors,
                     .probe = RecordedStream::Probe::FullCache,
                     .needs = Feature::Decompression},
                });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
