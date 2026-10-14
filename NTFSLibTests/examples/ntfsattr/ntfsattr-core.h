#pragma once

// Attribute dump behind the ntfsattr dialog. Plain C++ over the library, with
// no MFC types, so the unit tests can drive it without a window.

#include <string>
#include <string_view>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>

namespace NtfsAttrDump {

// The path a dump works on for the picked file. With directory set, that is
// the folder holding it. Empty when picked has no separator.
std::wstring TargetPath(std::wstring_view picked, bool directory);

// What one dump produced. message is empty on success. Otherwise it is the
// text the dialog shows, and dump stays empty.
struct DumpResult {
  std::wstring message;
  std::wstring dump;
};

// Dumps the raw header of every attribute of the file or directory at path, a
// full path such as C:\dir\file. A path ending in the drive, such as C:, dumps
// the root directory.
DumpResult DumpAttributes(
    NtfsBrowser::NtfsVolume<NtfsBrowser::Cache::Strategy::FullCache>& volume,
    std::wstring_view path);

}  // namespace NtfsAttrDump
