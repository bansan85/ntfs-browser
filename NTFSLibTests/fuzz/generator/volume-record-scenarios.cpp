#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/volume-record-scenarios.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using Boot = LegacyFixtures::Boot;

// The record of $Volume.
constexpr ULONGLONG volume_record = 3;

// Where the major version sits in the value of $VOLUME_INFORMATION, after 8
// reserved bytes.
constexpr std::size_t major_version_field = 8;

// A major version below the 3 that every supported volume has.
constexpr BYTE old_major_version = 2;

// Bit of MftMirrorDamage::in_mft that breaks the $Volume record.
constexpr std::uint32_t volume_record_bit = 1U << volume_record;

// A volume that says it is NTFS 2.1.
std::vector<BYTE> VersionBelowThree() {
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImage();
  const std::size_t attr = RecordEdit::Attribute(
      image, volume_record, RecordEdit::type_volume_information);
  RecordEdit::StoreByte(
      image, RecordEdit::ValueOffset(image, attr) + major_version_field,
      old_major_version);
  return image;
}

// A $Volume record that cannot be read, with its copy in the $MFT mirror sound.
std::vector<BYTE> CorruptMirrorIntact() {
  return FakeImage::BuildFakeNtfsImageWithMftMirror(
      {.in_mft = volume_record_bit});
}

}  // namespace

void VolumeRecordScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry, {
                    {.name = "volume_information_version_below_three",
                     .make = VersionBelowThree},
                    {.name = "volume_record_corrupt_mirror_intact",
                     .make = CorruptMirrorIntact,
                     .boot = Boot::Current},
                });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
