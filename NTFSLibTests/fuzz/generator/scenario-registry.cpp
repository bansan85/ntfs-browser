#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/resource-locator.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void ScenarioRegistry::Add(Scenario scenario) {
  if (Find(scenario.name) != nullptr) {
    throw std::logic_error("two scenarios are called " + scenario.name);
  }
  scenarios_.push_back(std::move(scenario));
}

void ScenarioRegistry::AddRecorded(
    std::string name, std::function<std::vector<BYTE>()> make_image,
    const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
    RecordedStream::Probe probe, Feature needs) {
  Add({.name = std::move(name),
       .needs = needs,
       .make = [make_image = std::move(make_image), options,
                probe](const ResourceLocator& /*resources*/) {
         return RecordedStream::Record(make_image(), options, probe);
       }});
}

const std::vector<Scenario>& ScenarioRegistry::All() const {
  return scenarios_;
}

const Scenario* ScenarioRegistry::Find(std::string_view name) const {
  const auto found = std::ranges::find(scenarios_, name, &Scenario::name);
  return found == scenarios_.end() ? nullptr : &*found;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
