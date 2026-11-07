#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/attribute-list-fixture-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace FakeImage = Test::FakeImage;

void AttributeListFixtureScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry,
      {
          {.name = "attribute_list_split_attribute",
           .make =
               FakeImage::BuildFakeNtfsImageWithSplitAttributeListAttribute},
      });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
