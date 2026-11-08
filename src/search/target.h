#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// A scan target split into the thing that holds the volume and the path inside
// the volume.
struct TargetParts {
  enum class Kind : std::uint8_t {
    // An image file, or a device on a POSIX host.
    Image,
    // A drive of Windows: source holds its letter.
    Drive
  };

  Kind kind{Kind::Image};
  // The path of the image file or device, or the drive letter.
  std::wstring source;
  // The names of the folders to enter below the root of the volume, in order.
  // Empty: the whole volume.
  std::vector<std::wstring> inside;
};

// Splits target, such as "/dev/sda1/Windows", "c:\image.img\Windows" or
// "c:\Windows". The source is the longest leading part that names an existing
// regular file, or on a POSIX host a block device. Else, on Windows, a drive
// letter followed by a colon names a drive. Both '\' and '/' separate the
// inside path, and empty parts are left out. The first rule that matches wins,
// without a look at the content: a file that holds no NTFS volume is a target
// that fails to open, and the drive is not tried then. Nothing when no rule
// matches.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<TargetParts>
    SplitTarget(std::wstring_view target);

// A volume opened from the parts of a target.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
struct OpenedVolume {
  // Null when the volume could not be opened or holds no NTFS: the log says
  // why.
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::NtfsVolume<S>> volume;
};

// Opens the volume that parts names, with the given options. Logs the reason
// at Error level when it fails.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY OpenedVolume<S>
    OpenVolume(const TargetParts& parts,
               const NTFS_BROWSER_NAMESPACE::VolumeOptions& options);

}  // namespace NTFS_BROWSER_NAMESPACE::Search
