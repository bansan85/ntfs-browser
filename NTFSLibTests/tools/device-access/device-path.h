#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace NtfsDevice {

// A command-line target split into the volume source and the directory
// inside the volume.
struct DeviceTarget {
  // An existing image file or, on POSIX, an existing block device.
  std::filesystem::path device;
  // The rest of the target, with '\' as the separator and no leading or
  // trailing one. Empty means the volume root.
  std::wstring directory;
};

// Splits target, such as "/dev/sda1/Windows" or "c:\image.img\Windows", into
// the device and the directory. The device is the longest leading part that
// names a regular file, or on POSIX also a block device. Both '/' and '\'
// separate on Windows; only '/' does on POSIX, where '\' is a legal file name
// character. Returns nothing when no leading part qualifies.
[[nodiscard]] std::optional<DeviceTarget>
    SplitDeviceTarget(std::wstring_view target);

}  // namespace NtfsDevice
