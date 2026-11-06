#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "bench-options.h"

namespace NtfsDevice {

class JsonWriter;

}  // namespace NtfsDevice

namespace NtfsBenchTool {

// The command line of one subcommand: the words that are not options, and the
// options as written.
struct Arguments {
  std::vector<std::wstring> words;
  // "--name=value" as {name, value}, "--name" as {name, ""}. The options of
  // NtfsDevice::BenchOptions (--profile, --tune, --disk...) are parsed apart.
  std::vector<std::pair<std::string, std::string>> options;
  NtfsDevice::BenchOptions bench;

  [[nodiscard]] bool Has(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> Value(std::string_view name) const;
  // The option as a whole number, or the fallback when it is absent. Sets
  // malformed when it is present and not a whole number.
  [[nodiscard]] std::uint64_t Unsigned(std::string_view name,
                                       std::uint64_t fallback,
                                       bool& malformed) const;
};

// Splits words and options. False, with the reason on stderr, when a
// benchmark option is malformed.
[[nodiscard]] bool ParseArguments(const std::vector<std::wstring>& arguments,
                                  Arguments& parsed);

// Volume options with the profile and the --tune settings of args applied for
// the disk behind target. Prints the reason and returns nothing when they fail.
[[nodiscard]] std::optional<NTFS_BROWSER_NAMESPACE::VolumeOptions>
    TunedOptions(const Arguments& args, std::wstring_view target);

// Opens the production reader on target: a drive letter ("c:"), a device or an
// image file, with the given read-ahead caps. Null, after saying why on
// stderr, when it cannot.
[[nodiscard]] std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> OpenReader(
    std::wstring_view target,
    const std::array<NTFS_BROWSER_NAMESPACE::Tuning::ReaderTuning,
                     NTFS_BROWSER_NAMESPACE::Tuning::pattern_count>& caps);

// Writes json to the file --json (or --out) names, or to stdout without it.
// Returns the exit code.
int EmitJson(const Arguments& args, NtfsDevice::JsonWriter& json);

// Empties the system file cache: flushes the modified pages, then drops the
// standby list on Windows, and the page cache on Linux. Needs administrator
// rights. False, with the reason in message, when it cannot.
[[nodiscard]] bool PurgeSystemCache(std::string& message);

// The subcommands. Each returns the exit code of the process.
int ReplayCommand(const Arguments& args);
int DiskBenchCommand(const Arguments& args);
int SimulateCommand(const Arguments& args);
int KnobsCommand(const Arguments& args);
int DiskIdCommand(const Arguments& args);
int PurgeCommand(const Arguments& args);
int WakeCommand(const Arguments& args);
int SampleCommand(const Arguments& args);

}  // namespace NtfsBenchTool
