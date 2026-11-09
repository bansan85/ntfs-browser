#include "generator-app.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/command-line.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>

#include "byte-writer.h"
#include "io/extended-path.h"
#include "resource-locator.h"
#include "sparse-image.h"
#include "stream-comparator.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

GeneratorApp::GeneratorApp(const ScenarioRegistry& registry, std::ostream& out,
                           std::ostream& err)
    : registry_(&registry), out_(&out), err_(&err) {}

int GeneratorApp::Run(const Options& options) const {
  if (options.mode == Options::Mode::Help) {
    CommandLine::Usage(*err_);
    return 1;
  }
  if (options.mode == Options::Mode::Predict) {
    return Predict(options);
  }
  std::vector<const Scenario*> selected;
  if (!Select(options, selected)) {
    return 1;
  }
  switch (options.mode) {
    case Options::Mode::List:
      List(selected);
      return 0;
    case Options::Mode::Out:
      return Generate(options, selected);
    case Options::Mode::Seeds:
      // The traces sit in a subfolder: they are no fuzz input.
      std::erase_if(selected, [](const Scenario* scenario) {
        return scenario->name.find('/') != std::string::npos;
      });
      return Generate(options, selected);
    case Options::Mode::Verify:
      return Verify(options, selected);
    case Options::Mode::Help:
    case Options::Mode::Predict:
      break;
  }
  return 1;
}

int GeneratorApp::Predict(const Options& options) const {
  try {
    const ResourceLocator resources(options.resources);
    std::ifstream layers_file(options.layers, std::ios::binary);
    if (!layers_file) {
      throw std::runtime_error("cannot read " + options.layers.string());
    }
    const std::string layers((std::istreambuf_iterator<char>(layers_file)),
                             std::istreambuf_iterator<char>());
    const SparseImage image =
        SparseImage::Load(layers, {}, [&resources](std::string_view name) {
          return resources.Read(name);
        });

    std::ifstream intervals(options.intervals);
    if (!intervals) {
      throw std::runtime_error("cannot read " + options.intervals.string());
    }
    std::vector<BYTE> predicted;
    ULONGLONG start = 0;
    ULONGLONG end = 0;
    while (intervals >> start >> end) {
      const std::size_t before = predicted.size();
      predicted.resize(before + gsl::narrow<std::size_t>(end - start));
      image.Read(start, std::span<BYTE>(predicted).subspan(before));
    }
    const ByteWriter writer(options.output.parent_path());
    writer.Write(options.output.filename().generic_string(), predicted);
  } catch (const std::exception& error) {
    *err_ << error.what() << "\n";
    return 1;
  }
  return 0;
}

bool GeneratorApp::Select(const Options& options,
                          std::vector<const Scenario*>& selected) const {
  if (options.only.empty()) {
    for (const Scenario& scenario : registry_->All()) {
      selected.push_back(&scenario);
    }
    return true;
  }
  bool known = true;
  for (const std::string& name : options.only) {
    const Scenario* const scenario = registry_->Find(name);
    if (scenario == nullptr) {
      *err_ << "unknown scenario " << name << "\n";
      known = false;
    } else {
      selected.push_back(scenario);
    }
  }
  return known;
}

void GeneratorApp::List(const std::vector<const Scenario*>& selected) const {
  std::vector<const Scenario*> sorted = selected;
  std::ranges::sort(sorted, {}, &Scenario::name);
  for (const Scenario* const scenario : sorted) {
    *out_ << scenario->name;
    if (scenario->needs != Feature::None) {
      *out_ << " (needs " << BuildFeatures::Name(scenario->needs) << ")";
    }
    *out_ << "\n";
  }
}

int GeneratorApp::Generate(const Options& options,
                           const std::vector<const Scenario*>& selected) const {
  const ResourceLocator resources(options.resources);
  const ByteWriter writer(options.folder);
  int failed = 0;
  for (const Scenario* const scenario : selected) {
    if (!BuildFeatures::Has(scenario->needs)) {
      *out_ << scenario->name << ": skipped (feature off)\n";
      continue;
    }
    try {
      writer.Write(scenario->name, scenario->make(resources));
    } catch (const std::exception& error) {
      *err_ << scenario->name << ": " << error.what() << "\n";
      ++failed;
    }
  }
  return failed == 0 ? 0 : 1;
}

int GeneratorApp::Verify(const Options& options,
                         const std::vector<const Scenario*>& selected) const {
  const ResourceLocator resources(options.resources);
  std::array<int, 4> counts{};
  int skipped = 0;
  int failed = 0;
  for (const Scenario* const scenario : selected) {
    if (!BuildFeatures::Has(scenario->needs)) {
      *out_ << scenario->name << ": skipped (feature off)\n";
      ++skipped;
      continue;
    }
    try {
      const Comparison comparison = StreamComparator::Compare(
          scenario->make(resources),
          options.folder / std::filesystem::path(scenario->name));
      *out_ << scenario->name << ": " << StreamComparator::Describe(comparison)
            << "\n";
      ++counts.at(static_cast<std::size_t>(comparison.verdict));
    } catch (const std::exception& error) {
      *err_ << scenario->name << ": " << error.what() << "\n";
      ++failed;
    }
  }

  // The files of the folder that no scenario makes.
  int uncovered = 0;
  if (options.only.empty()) {
    std::set<std::string> known;
    for (const Scenario& scenario : registry_->All()) {
      known.insert(scenario.name);
    }
    std::error_code error;
    const std::filesystem::path folder =
        Io::ToExtendedLengthFsPath(options.folder);
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(folder, error)) {
      if (entry.is_regular_file() &&
          !known.contains(
              entry.path().lexically_relative(folder).generic_string())) {
        ++uncovered;
      }
    }
  }

  *out_ << "identical "
        << counts.at(static_cast<std::size_t>(Verdict::Identical))
        << ", prefix " << counts.at(static_cast<std::size_t>(Verdict::Prefix))
        << ", different "
        << counts.at(static_cast<std::size_t>(Verdict::Different))
        << ", missing " << counts.at(static_cast<std::size_t>(Verdict::Missing))
        << ", skipped " << skipped << ", errors " << failed
        << ", files without a scenario " << uncovered << "\n";
  const bool bad =
      counts.at(static_cast<std::size_t>(Verdict::Different)) != 0 ||
      counts.at(static_cast<std::size_t>(Verdict::Missing)) != 0 || failed != 0;
  return bad ? 1 : 0;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
