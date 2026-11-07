#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/legacy-builder-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/legacy-geometry.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;
using Probe = RecordedStream::Probe;

// The root directory record, which the old builders sized without padding.
constexpr ULONGLONG root_record = 5;

// Cuts the root record to the size the old builders gave it.
void PackRoot(Image& image) {
  LegacyGeometry::PackLastAttribute(image, root_record);
}

const std::vector<LegacyFixtures::Damaged>& Inputs() {
  static const std::vector<LegacyFixtures::Damaged> inputs = {
      {.name = "attr_offset_exceeds_record_size",
       .base = FakeImage::BuildFakeNtfsImageWithAttrOffsetOutOfBounds,
       .early = true},
      {.name = "corrupt_mft_record_volume_ok",
       .base = FakeImage::BuildFakeNtfsImageWithCorruptMftRecord,
       .early = true},
      {.name = "attribute_walk_no_end_marker",
       .base = FakeImage::BuildFakeNtfsImageWithNoEndMarker,
       .probe = Probe::FullCache},
      {.name = "find_stream_named_data",
       .base = FakeImage::BuildFakeNtfsImageWithNamedDataStream,
       .damage = PackRoot},
      {.name = "compressed_index_allocation_missing_compressed_size",
       .base = FakeImage::
           BuildFakeNtfsImageWithMissingCompressedSizeIndexAllocation,
       .damage = PackRoot},
      {.name = "standard_information_minimal_size",
       .base = FakeImage::BuildFakeNtfsImageWithLegacyStandardInformation},
      {.name = "volume_information_minimal_size",
       .base = FakeImage::BuildFakeNtfsImage},
      {.name = "root_record_parse_failure",
       .base = FakeImage::BuildFakeNtfsImageWithCorruptRootRecord},
  };
  return inputs;
}

}  // namespace

void LegacyBuilderScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::RegisterDamaged(registry, Inputs());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
