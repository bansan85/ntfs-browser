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

// What a command-line target names, for a tool that reads the whole thing and
// takes no directory part.
enum class TargetKind {
  // A drive on Windows: "c", "c:", "c:\" or "c:/". Nothing else is a drive: a
  // path that merely starts with a letter and a colon is a file.
  Drive,
  // A device: a path that starts with "\\.\" on Windows, an existing block
  // device on POSIX. The existence of the first is not tested.
  Device,
  // An existing regular file, an image.
  File,
  // Anything else: a path that does not exist, or that is a directory.
  Missing
};

// The drive letter of target when target is "c", "c:", "c:\" or "c:/". The
// letter is one of the ASCII alphabet. Nothing for any other string. It is a
// test of the text, on every platform: only Windows has drives.
[[nodiscard]] std::optional<wchar_t> DriveLetterOf(std::wstring_view target);

// What target names. On Windows a drive (see DriveLetterOf()) and a "\\.\"
// path are not looked up; every other target is a file only if it exists.
[[nodiscard]] TargetKind ClassifyTarget(std::wstring_view target);

// Splits target, such as "/dev/sda1/Windows" or "c:\image.img\Windows", into
// the device and the directory. The device is the longest leading part that
// names a regular file, or on POSIX also a block device. Both '/' and '\'
// separate on Windows; only '/' does on POSIX, where '\' is a legal file name
// character. Returns nothing when no leading part qualifies.
[[nodiscard]] std::optional<DeviceTarget>
    SplitDeviceTarget(std::wstring_view target);

}  // namespace NtfsDevice
