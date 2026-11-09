#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>

#include "cache/strategy.h"
#include "log/log.h"
#include "ntfs-volume.h"
#include "volume-options.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// The options every tool that opens one volume takes.
struct CommonArguments {
  NTFS_BROWSER_NAMESPACE::Log::Config log_config;
  NTFS_BROWSER_NAMESPACE::VolumeOptions volume_options;
  // Set by --help or -h.
  bool help = false;
};

// Reads arg when it is --help (-h), --log=..., --deleted or --recover, and
// stores it in args. Malformed is a bad --log. Any other arg is NotMine.
[[nodiscard]] OptionResult ParseCommonOption(std::wstring_view arg,
                                             CommonArguments& args);

// The usage lines of the options ParseCommonOption() reads, each ending in a
// newline.
[[nodiscard]] std::string CommonOptionsUsage();

// The parts of a path inside a volume, split on '\' and '/'. Empty parts are
// left out, so the empty path is the root and "a\b\" has the parts a and b.
[[nodiscard]] std::vector<std::wstring_view>
    SplitInsidePath(std::wstring_view path);

// A volume opened from a command-line target.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
struct OpenedVolume {
  // Null when the target could not be opened. error says why.
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::NtfsVolume<S>> volume;
  // The rest of the target: the path inside the volume, '\' separated, with
  // no leading or trailing separator. Empty means the root.
  std::wstring inside;
  std::wstring error;
  // What the volume is read from: the image file, the device, or on Windows the
  // root of the drive ("c:\"). Set even when the volume does not open.
  std::filesystem::path source;
};

// Opens the volume that target names. The target is an image file or a device
// followed by the path inside the volume ("/dev/sda1/Windows",
// "c:\image.img\Windows"). On Windows it can be a drive letter instead
// ("c:\windows"). A leading part that names a file but holds no NTFS volume
// does not stop a drive letter from being tried.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] OpenedVolume<S>
    OpenVolumeFromTarget(std::wstring_view target,
                         const NTFS_BROWSER_NAMESPACE::VolumeOptions& options);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
