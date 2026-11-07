#pragma once

#include <ntfs-browser/namespace.h>

#ifndef _WIN32

  #include <filesystem>
  #include <optional>

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// Whether the block device major:minor spins, read from the sysfs tree under
// root (queue/rotational of the disk, or of the disk that holds the partition).
// Nothing where root has no such device, or where its disk does not say: a file
// on a network share or on a pseudo file system has no block device.
[[nodiscard]] std::optional<bool>
    RotationalOfBlockDevice(unsigned major, unsigned minor,
                            const std::filesystem::path& root);

// The sysfs tree that PosixDeviceDiskReader asks about the volume of an image
// file: "/sys" unless SetSysfsRootForTests() replaced it.
[[nodiscard]] const std::filesystem::path& SysfsRoot();

// Replaces the sysfs tree that SysfsRoot() gives. An empty path restores
// "/sys". The unit tests point it at a folder with no tree before main(), so
// that no result depends on the disk of the machine.
void SetSysfsRootForTests(std::filesystem::path root);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess

#endif  // !_WIN32
