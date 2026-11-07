#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// Says which volume of the host holds path. Two paths with the same text sit
// on the same volume. The text means nothing else.
//
// Windows: the GUID name of the volume that GetVolumePathName() finds, or its
// serial number when the volume has no GUID name. A drive root or "\\.\X:"
// names the volume of that drive. Other "\\.\" devices have no answer.
// POSIX: the device number of the file system that holds the path, or the
// device number of the node when path is a block device.
//
// A path that does not exist takes the answer of its nearest parent that does.
// Nothing is returned when the host does not tell.
[[nodiscard]] std::optional<std::string>
    HostVolumeId(const std::filesystem::path& path);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
