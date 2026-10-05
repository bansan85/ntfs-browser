#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

namespace NtfsCompare
{

// What OpenVolumeFor() resolved: two independently-opened volumes (FullCache
// and NoCache each own their own IDiskReader and state) plus the path of
// the target directory relative to the volume's root, in this tool's own
// "/"-joined convention (matching every Listing's own path keys).
struct VolumeHandles
{
  std::unique_ptr<NtfsBrowser::NtfsVolume<NtfsBrowser::Strategy::FullCache>>
      full_cache;
  std::unique_ptr<NtfsBrowser::NtfsVolume<NtfsBrowser::Strategy::NoCache>>
      no_cache;
  std::wstring relative_path;
};

// Opens the NTFS volume backing target's directory: by drive letter on
// Windows, or by locating and opening the raw block device behind target's
// mount point on Linux (see raw-device-disk-reader.h). Prints a diagnostic to
// stderr and returns std::nullopt on any failure (not a real NTFS volume,
// permission denied opening the drive/device, target not under a real mount,
// ...): the caller then skips the three library-based methods but keeps
// running the OS-level ones.
[[nodiscard]] std::optional<VolumeHandles>
    OpenVolumeFor(const std::filesystem::path& target);

}  // namespace NtfsCompare
