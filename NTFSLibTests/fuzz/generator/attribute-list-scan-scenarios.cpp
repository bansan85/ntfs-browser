#include <ntfs-browser/win-types.h>

#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/attribute-list-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// Slots of the list fixture of the waiting scan: five turns of the kinds of
// slot, past the 16 system files. Each turn has 14 slots.
constexpr ULONGLONG system_slots = 16;
constexpr ULONGLONG slots_per_turn = 14;
constexpr ULONGLONG waiting_scan_turns = 5;
constexpr ULONGLONG waiting_scan_slots =
    system_slots + (slots_per_turn * waiting_scan_turns);

// One input whose volume is strict: a foreign extension record rejects its
// file.
struct StrictVariant {
  std::string name;
  FakeImage::FakeListedExtension extension;
  FakeImage::FakeListResidency residency;
};

// One input where a freed file's non-resident list holds other bytes.
struct OverwrittenVariant {
  std::string name;
  FakeImage::FakeListBytes bytes;
};

// The ways a list spreads a file over extension records, residency aside.
const std::vector<StrictVariant>& StrictVariants() {
  static const std::vector<StrictVariant> variants{
      {"attribute_list_scan_both_sides",
       FakeImage::FakeListedExtension::Genuine,
       FakeImage::FakeListResidency::Resident},
      {"attribute_list_scan_foreign_extension",
       FakeImage::FakeListedExtension::Foreign,
       FakeImage::FakeListResidency::Resident},
      {"attribute_list_scan_non_resident",
       FakeImage::FakeListedExtension::Genuine,
       FakeImage::FakeListResidency::NonResident}};
  return variants;
}

// The bytes another file leaves in the list of a freed file.
const std::vector<OverwrittenVariant>& OverwrittenVariants() {
  static const std::vector<OverwrittenVariant> variants{
      {"attribute_list_overwritten_invalid_type",
       FakeImage::FakeListBytes::InvalidTypeFirst},
      {"attribute_list_overwritten_cut_entry",
       FakeImage::FakeListBytes::CutLastEntry}};
  return variants;
}

}  // namespace

void AttributeListScanScenarios::Register(ScenarioRegistry& registry) {
  // Strict: a foreign extension record rejects its file.
  const NTFS_BROWSER_NAMESPACE::VolumeOptions strict{};
  for (const StrictVariant& variant : StrictVariants()) {
    registry.AddRecorded(
        variant.name,
        [&variant] {
          return FakeImage::BuildFakeNtfsImageWithMftTreeAttributeList(
              variant.extension, variant.residency);
        },
        strict);
  }
  registry.AddRecorded(
      "attribute_list_multi_cluster",
      [] {
        return FakeImage::BuildFakeNtfsImageWithMultiClusterAttributeList();
      },
      strict);

  // Recovering with deleted items shown: only then does the volume read the
  // list of a freed file.
  const auto recovering = RecordedStream::Recovering();
  for (const OverwrittenVariant& variant : OverwrittenVariants()) {
    registry.AddRecorded(
        variant.name,
        [&variant] {
          return FakeImage::BuildFakeNtfsImageWithMftTreeAttributeList(
              FakeImage::FakeListedExtension::Genuine,
              FakeImage::FakeListResidency::NonResident,
              FakeImage::FakeListedState::Freed, variant.bytes);
        },
        recovering);
  }

  // The scan that the thread probe repeats, recorded from the scan itself: the
  // main probes read in another order.
  const auto waiting_image = [] {
    return FakeImage::BuildFakeNtfsImageWithListMftScan(waiting_scan_slots);
  };
  registry.AddRecorded("scan_threads_waiting_strict", waiting_image, strict,
                       RecordedStream::Probe::Scan);
  registry.AddRecorded("scan_threads_waiting_recovering", waiting_image,
                       recovering, RecordedStream::Probe::Scan);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
