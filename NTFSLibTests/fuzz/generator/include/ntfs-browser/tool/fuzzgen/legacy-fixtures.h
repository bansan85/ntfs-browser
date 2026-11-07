#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The saved inputs of the 2026-10 fixtures: a fake volume damaged in one
// field, whose boot sector has the form the first recorder gave it.
class LegacyFixtures {
 public:
  // The forms of boot sector the old inputs carry.
  enum class Boot : std::uint8_t {
    // As the builder makes it.
    Current,
    // total_sectors is 0 and the two bytes of the end marker are swapped.
    UnsetTotalSectors,
    // total_sectors is the largest value, as the fuzzer patches it.
    PatchedTotalSectors
  };

  // One saved input.
  struct Fixture {
    // The file name.
    std::string name;
    // The image, before the boot sector takes its form.
    std::function<std::vector<BYTE>()> make;
    Boot boot = Boot::UnsetTotalSectors;
    // The volume options, and the probe whose reads are recorded.
    NTFS_BROWSER_NAMESPACE::VolumeOptions options =
        RecordedStream::Recovering();
    RecordedStream::Probe probe = RecordedStream::Probe::Main;
    Feature needs = Feature::None;
  };

  // Adds one scenario per fixture.
  static void Register(ScenarioRegistry& registry,
                       const std::vector<Fixture>& fixtures);

  // Gives the boot sector of image the form boot.
  static void ApplyBoot(std::vector<BYTE>& image, Boot boot);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
