#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "device-path.h"

namespace NtfsCompare {

// Options for every volume this tool opens. They are the library defaults,
// written out so the comparison does not depend on them. include_deleted
// MUST stay off: the OS walks never list freed entries, so the reference
// must not either. recover_errors stays off too, so a damaged entry is
// hidden from every listing the same way.
inline constexpr NTFS_BROWSER_NAMESPACE::VolumeOptions
    comparison_volume_options{
        .include_deleted = false,
        .recover_errors = false,
};

// What OpenVolumeFor() resolved: two independently-opened volumes (FullCache
// and NoCache each own their own IDiskReader and state) plus the path of
// the target directory relative to the volume's root, in this tool's own
// "/"-joined convention (matching every Listing's own path keys).
struct VolumeHandles {
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>>
      full_cache;
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>>
      no_cache;
  std::wstring relative_path;
  // Where the volume is mounted, when the target named a device and the
  // operating system has it mounted. Unset: the operating-system listings are
  // skipped.
  std::optional<std::filesystem::path> mount_point;
  // The filesystem type of that mount (eg. "fuse", "ntfs3"), empty when there
  // is none. It tells which driver rendered the operating system's names.
  std::string mount_fstype;
};

// Opens the NTFS volume backing target's directory: by drive letter on
// Windows, or by locating and opening the raw block device behind target's
// mount point on Linux (see posix-device-disk-reader.h). Prints a diagnostic to
// stderr and returns std::nullopt on any failure (not a real NTFS volume,
// permission denied opening the drive/device, target not under a real mount,
// ...): the caller then skips the three library-based methods but keeps
// running the OS-level ones.
[[nodiscard]] std::optional<VolumeHandles>
    OpenVolumeFor(const std::filesystem::path& target);

#ifndef _WIN32

// A mount table entry: where a device is mounted, and with which driver.
struct MountEntry {
  std::filesystem::path mount_point;
  std::string fstype;
};

// Searches mounts_file, in the format of /proc/mounts, for the entry whose
// source is device (after resolving symbolic links). Returns nothing when the
// device is not mounted. The file is a parameter so a test can supply one.
[[nodiscard]] std::optional<MountEntry>
    FindMountOfDevice(const std::filesystem::path& device,
                      const char* mounts_file = "/proc/mounts");

// Opens the NTFS volume that target names, an image file or a block device,
// and looks for its mount. Prints a diagnostic to stderr and returns
// std::nullopt on any failure; a read-access failure says that access is
// needed.
[[nodiscard]] std::optional<VolumeHandles>
    OpenDeviceVolume(const NtfsDevice::DeviceTarget& target);

#endif  // !_WIN32

}  // namespace NtfsCompare
