#include "bench-options.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/tuning/knobs.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "disk-identity.h"
#include "json.h"
#include "text-convert.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace NtfsDevice {

namespace {

// Version of the profile layout this reads.
constexpr std::uint64_t profile_format = 1;

// The section of a profile that applies to every disk without a section of its
// own.
constexpr std::string_view any_disk = "*";

// Reads an unsigned decimal number. False for anything else.
bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
  if (text.empty()) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

// "M1" to "M4".
std::optional<Tuning::Pattern> ParsePattern(std::string_view text) {
  for (std::size_t index = 0; index < Tuning::pattern_count; index++) {
    const auto pattern = static_cast<Tuning::Pattern>(index);
    if (Tuning::PatternName(pattern) == text) {
      return pattern;
    }
  }
  return std::nullopt;
}

// Parses "name=value" or "name@Mk=value".
bool ParseTune(std::string_view text, TuneSetting& tune, std::string& error) {
  const std::size_t equals = text.rfind('=');
  if (equals == std::string_view::npos) {
    error = "--tune expects name=value";
    return false;
  }
  std::string_view name = text.substr(0, equals);
  if (const std::size_t at = name.find('@'); at != std::string_view::npos) {
    tune.pattern = ParsePattern(name.substr(at + 1));
    if (!tune.pattern) {
      error = "--tune names the pattern \"" + std::string(name.substr(at + 1)) +
              "\": it is M1, M2, M3 or M4";
      return false;
    }
    name = name.substr(0, at);
  }
  if (!ParseUnsigned(text.substr(equals + 1), tune.value)) {
    error = "--tune expects a whole number after the equals sign";
    return false;
  }
  tune.name = std::string(name);
  return true;
}

// Reads the whole file at path. False when it cannot be read.
bool ReadFile(const std::filesystem::path& path, std::string& text) {
  std::ifstream in(NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path),
                   std::ios::binary);
  if (!in) {
    return false;
  }
  text.assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

// Applies the knobs of one section object.
std::string ApplySection(const JsonValue& section, const std::string& where,
                         std::optional<Tuning::Pattern> pattern,
                         VolumeOptions& options,
                         std::vector<std::string>& applied) {
  if (section.kind != JsonValue::Kind::Object) {
    return where + " is not an object";
  }
  for (const auto& [name, value] : section.members) {
    if (value.kind != JsonValue::Kind::Number || value.number < 0 ||
        value.number != std::floor(value.number) ||
        value.number > static_cast<double>(std::uint64_t{1} << 53U)) {
      return where + "." + name + " is not a whole number";
    }
    const auto number = static_cast<std::uint64_t>(value.number);
    const std::string failure =
        Tuning::ApplyKnob(options, name, number, pattern);
    if (!failure.empty()) {
      // Only the tunable constants have a name: a guard rail has none.
      return where + ": " + failure +
             (Tuning::FindKnob(name) == nullptr
                  ? " (only constants of families A, B and C can be set)"
                  : "");
    }
    applied.push_back(name +
                      (pattern
                           ? "@" + std::string(Tuning::PatternName(*pattern))
                           : std::string()) +
                      "=" + std::to_string(number));
  }
  return {};
}

// Applies the section of one disk.
std::string ApplyDisk(const JsonValue& disk, const std::string& where,
                      VolumeOptions& options,
                      std::vector<std::string>& applied) {
  if (disk.kind != JsonValue::Kind::Object) {
    return where + " is not an object";
  }
  // The common section first, so that a pattern section overrides it.
  if (const JsonValue* common = disk.Find("common")) {
    if (std::string failure = ApplySection(*common, where + ".common",
                                           std::nullopt, options, applied);
        !failure.empty()) {
      return failure;
    }
  }
  for (std::size_t index = 0; index < Tuning::pattern_count; index++) {
    const auto pattern = static_cast<Tuning::Pattern>(index);
    const std::string name(Tuning::PatternName(pattern));
    if (const JsonValue* section = disk.Find(name)) {
      if (std::string failure = ApplySection(*section, where + "." + name,
                                             pattern, options, applied);
          !failure.empty()) {
        return failure;
      }
    }
  }
  return {};
}

// Checks the keys of a disk object: a typo MUST NOT pass as a missing value.
std::string CheckDiskKeys(const JsonValue& disk, const std::string& where) {
  constexpr std::array<std::string_view, 8> allowed{
      "label", "notes", "solid_state", "common", "M1", "M2", "M3", "M4"};
  for (const auto& [name, value] : disk.members) {
    bool known = false;
    for (const std::string_view key : allowed) {
      known = known || key == name;
    }
    if (!known) {
      return where + ": unknown key \"" + name + "\"";
    }
  }
  return {};
}

// Loads the profile at path and applies the section of disk_key.
std::string ApplyProfile(const std::filesystem::path& path,
                         const std::string& disk_key, VolumeOptions& options,
                         AppliedTuning& applied) {
  const std::string file = path.string();
  std::string text;
  if (!ReadFile(path, text)) {
    return "cannot read the profile " + file;
  }
  std::string error;
  const std::optional<JsonValue> root = ParseJson(text, error);
  if (!root) {
    return file + ": " + error;
  }
  if (root->kind != JsonValue::Kind::Object) {
    return file + ": the profile is not an object";
  }
  for (const auto& [name, value] : root->members) {
    if (name != "format" && name != "disks" && name != "generator") {
      return file + ": unknown key \"" + name + "\"";
    }
  }
  const JsonValue* format = root->Find("format");
  if (format == nullptr || format->kind != JsonValue::Kind::Number ||
      format->number != static_cast<double>(profile_format)) {
    return file + ": \"format\" MUST be " + std::to_string(profile_format);
  }
  const JsonValue* disks = root->Find("disks");
  if (disks == nullptr || disks->kind != JsonValue::Kind::Object) {
    return file + ": \"disks\" MUST be an object";
  }
  // Every section is checked, the disks the run does not use as well.
  for (const auto& [key, disk] : disks->members) {
    const std::string where = file + ": disks[\"" + key + "\"]";
    if (disk.kind != JsonValue::Kind::Object) {
      return where + " is not an object";
    }
    if (std::string failure = CheckDiskKeys(disk, where); !failure.empty()) {
      return failure;
    }
  }

  applied.profile_loaded = true;
  const JsonValue* section = disks->Find(disk_key);
  std::string used = disk_key;
  if (section == nullptr) {
    section = disks->Find(any_disk);
    used = std::string(any_disk);
  }
  if (section == nullptr) {
    return {};
  }
  applied.section_found = true;
  applied.disk_key = used;
  return ApplyDisk(*section, file + ": disks[\"" + used + "\"]", options,
                   applied.applied);
}

}  // namespace

OptionResult ParseBenchOption(std::wstring_view arg, BenchOptions& options,
                              std::string& error) {
  const std::string text = WideToUtf8(arg);
  const auto value_of =
      [&text](std::string_view name) -> std::optional<std::string_view> {
    if (text.size() > name.size() && text.starts_with(name) &&
        text[name.size()] == '=') {
      return std::string_view(text).substr(name.size() + 1);
    }
    return std::nullopt;
  };

  if (text == "--benchmark") {
    options.benchmark = true;
    return OptionResult::Accepted;
  }
  if (const auto value = value_of("--benchmark")) {
    if (value->empty()) {
      error =
          "--benchmark= needs a path: write --benchmark alone for the "
          "default one";
      return OptionResult::Malformed;
    }
    options.benchmark = true;
    options.summary_path =
        PathFromWide(arg.substr(std::string_view("--benchmark=").size()));
    return OptionResult::Accepted;
  }
  if (const auto value = value_of("--capture")) {
    if (value->empty()) {
      error = "--capture needs a path";
      return OptionResult::Malformed;
    }
    options.capture_path =
        PathFromWide(arg.substr(std::string_view("--capture=").size()));
    return OptionResult::Accepted;
  }
  if (const auto value = value_of("--stop-after")) {
    options.stop_after = Bench::ParsePhase(*value);
    if (!options.stop_after) {
      error = "--stop-after names the phase \"" + std::string(*value) +
              "\": it is open, pass1, pass2, flush, sweep, check, link, "
              "journal, walk, print or destroy";
      return OptionResult::Malformed;
    }
    return OptionResult::Accepted;
  }
  if (const auto value = value_of("--profile")) {
    if (value->empty()) {
      error = "--profile needs a path";
      return OptionResult::Malformed;
    }
    options.profile_path =
        PathFromWide(arg.substr(std::string_view("--profile=").size()));
    return OptionResult::Accepted;
  }
  if (const auto value = value_of("--disk")) {
    options.disk = std::string(*value);
    return OptionResult::Accepted;
  }
  if (const auto value = value_of("--tune")) {
    TuneSetting tune;
    if (!ParseTune(*value, tune, error)) {
      return OptionResult::Malformed;
    }
    options.tunes.push_back(std::move(tune));
    return OptionResult::Accepted;
  }
  return OptionResult::NotMine;
}

std::string_view BenchUsage() {
  return "  --benchmark[=path]  time the run, write a JSON summary and the "
         "read log (path.readlog)\n"
         "  --capture=path      write what the run reads into a sparse image\n"
         "  --stop-after=phase  skip the phases after open, pass1, pass2, "
         "flush, sweep, check, link, journal, walk, print or destroy\n"
         "  --profile=path      set the tunable constants from a profile\n"
         "  --disk=key          take that disk's section of the profile\n"
         "  --tune=name[@Mk]=v  set one tunable constant, after the profile\n";
}

std::string ApplyTuning(const BenchOptions& bench, std::wstring_view target,
                        VolumeOptions& options, AppliedTuning& applied) {
  if (!bench.profile_path.empty()) {
    std::string key = bench.disk;
    if (key.empty()) {
      if (const std::optional<DiskIdentity> identity = IdentifyDisk(target)) {
        key = identity->key;
      }
    }
    if (std::string failure =
            ApplyProfile(bench.profile_path, key, options, applied);
        !failure.empty()) {
      return failure;
    }
  }
  for (const TuneSetting& tune : bench.tunes) {
    const std::string failure =
        Tuning::ApplyKnob(options, tune.name, tune.value, tune.pattern);
    if (!failure.empty()) {
      return "--tune: " + failure;
    }
    applied.applied.push_back(
        tune.name +
        (tune.pattern ? "@" + std::string(Tuning::PatternName(*tune.pattern))
                      : std::string()) +
        "=" + std::to_string(tune.value));
  }
  return {};
}

}  // namespace NtfsDevice
