#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

// Turns path into a Windows extended-length path, which Win32 file calls take
// up to about 32 767 UTF-16 units, where an ordinary path stops at 260. The
// path is made absolute and normalised first (GetFullPathNameW: "." and "..",
// forward slashes, trailing dots and spaces), then prefixed with \\?\ or, for
// a UNC path, \\?\UNC\. A path that already starts with \\?\ or \\.\ is
// returned as it is: the first is already extended, the second names a device
// (\\.\PhysicalDrive0, \\.\C:), which MUST NOT take the prefix. The prefix
// alone lifts the limit: the LongPathsEnabled registry switch and a
// longPathAware manifest are not needed. A path the system cannot resolve is
// returned unchanged. Off Windows, every path is returned unchanged.
[[nodiscard]] NTFS_BROWSER_EXPORT std::wstring
    ToExtendedLengthPath(std::wstring_view path);

// Same, for a std::filesystem::path.
[[nodiscard]] NTFS_BROWSER_EXPORT std::filesystem::path
    ToExtendedLengthFsPath(const std::filesystem::path& path);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
