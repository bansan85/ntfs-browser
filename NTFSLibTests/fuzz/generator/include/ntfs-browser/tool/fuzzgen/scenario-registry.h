#pragma once

#include <ntfs-browser/win-types.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The scenarios of the generator, by name.
class ScenarioRegistry {
 public:
  // Adds scenario. Throws std::logic_error when the name is already taken.
  void Add(Scenario scenario);

  // Adds the scenario whose file is the stream that probe records from the
  // image that make_image builds, with the volume opened under options.
  void AddRecorded(std::string name,
                   std::function<std::vector<BYTE>()> make_image,
                   const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                   RecordedStream::Probe probe = RecordedStream::Probe::Main,
                   Feature needs = Feature::None);

  // The scenarios, in the order they were added.
  [[nodiscard]] const std::vector<Scenario>& All() const;

  // The scenario called name, or nullptr.
  [[nodiscard]] const Scenario* Find(std::string_view name) const;

 private:
  std::vector<Scenario> scenarios_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
