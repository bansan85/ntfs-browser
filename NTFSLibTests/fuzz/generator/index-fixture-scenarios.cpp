#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/index-fixture-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace FakeImage = Test::FakeImage;
using Boot = LegacyFixtures::Boot;

void IndexFixtureScenarios::Register(ScenarioRegistry& registry) {
  // Both inputs are the reads of a FullCache volume, which takes whole blocks.
  const NTFS_BROWSER_NAMESPACE::VolumeOptions strict{};
  LegacyFixtures::Register(
      registry,
      {
          {.name = "index_allocation_split_runs",
           .make = FakeImage::BuildFakeNtfsImageWithSplitIndexAllocation,
           .boot = Boot::PatchedTotalSectors,
           .options = strict,
           .probe = RecordedStream::Probe::FullCache},
          {.name = "index_orphan_scan_huge_block_count",
           .make = FakeImage::BuildFakeNtfsImageWithHugeOrphanScanBlockCount,
           .boot = Boot::PatchedTotalSectors,
           .options = strict,
           .probe = RecordedStream::Probe::FullCache},
      });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
