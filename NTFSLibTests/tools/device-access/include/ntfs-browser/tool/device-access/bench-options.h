#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "bench/bench.h"
#include "tuning/tuning.h"
#include "volume-options.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// One --tune=name[@Mk]=value setting.
struct TuneSetting {
  std::string name;
  std::optional<NTFS_BROWSER_NAMESPACE::Tuning::Pattern> pattern;
  std::uint64_t value{0};
};

// What the command line asks of the instrumentation and of the tuning. The
// three console tools share it.
struct BenchOptions {
  // --benchmark[=<path>]: time the run, and write a JSON summary at
  // summary_path and the read log at the same path plus ".readlog".
  bool benchmark{false};
  std::filesystem::path summary_path{"ntfs-benchmark.json"};
  // --capture=<path>: write what the run reads into a sparse image.
  std::filesystem::path capture_path;
  // --stop-after=<phase>: skip the phases that follow.
  std::optional<NTFS_BROWSER_NAMESPACE::Bench::Phase> stop_after;
  // --profile=<path>: set the tunable constants from a profile, for the disk
  // of the target, or the one --disk names.
  std::filesystem::path profile_path;
  std::string disk;
  // --tune=...: set one constant, after the profile.
  std::vector<TuneSetting> tunes;
  // --media=ssd|hdd|unknown: the kind of device the volume plans its reads
  // for, instead of the one the device says. A run on a copy of a volume
  // that sits on another drive says the kind of the original.
  std::optional<NTFS_BROWSER_NAMESPACE::Tuning::MediaKind> media;

  // Whether the run starts a session: timing or capturing.
  [[nodiscard]] bool WantsSession() const {
    return benchmark || !capture_path.empty();
  }
};

enum class OptionResult : std::uint8_t { NotMine, Accepted, Malformed };

// Reads arg when it is one of --benchmark, --capture, --stop-after, --profile,
// --disk, --tune or --media. When it is malformed, error says why.
[[nodiscard]] OptionResult ParseBenchOption(std::wstring_view arg,
                                            BenchOptions& options,
                                            std::string& error);

// The name a summary gives a media kind: "unknown", "solid_state" or
// "rotational".
[[nodiscard]] std::string_view
    MediaKindName(NTFS_BROWSER_NAMESPACE::Tuning::MediaKind kind);

// The usage lines of these options, one per line.
[[nodiscard]] std::string_view BenchUsage();

// What ApplyTuning() did, for the summary.
struct AppliedTuning {
  // The disk the profile section was taken for: empty when no profile.
  std::string disk_key;
  bool profile_loaded{false};
  // False when the profile has no section for the disk: nothing was applied
  // from it.
  bool section_found{false};
  // Settings applied, profile first, then --tune, as "name=value" text.
  std::vector<std::string> applied;
};

// Applies the profile and the --tune settings to options.tuning, and --media to
// options.media_kind, for the disk behind target (a drive letter, a device or
// an image file). Returns an empty
// string, or the reason the profile cannot be used: an unreadable file, an
// unknown key, a value out of range.
[[nodiscard]] std::string
    ApplyTuning(const BenchOptions& bench, std::wstring_view target,
                NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                AppliedTuning& applied);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
