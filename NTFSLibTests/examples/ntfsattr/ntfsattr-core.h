#pragma once

// Attribute dump behind the ntfsattr dialog. Plain C++ over the library, with
// no MFC types, so the unit tests can drive it without a window.

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

namespace NtfsAttrDump {

// Appends a hex dump of datalen bytes at data to lines, 16 bytes a row. Every
// row ends in CRLF. Zero bytes append nothing, and data is not read.
void AppendData(std::wstring& lines, const BYTE* data, std::uint32_t datalen);

// Name of an attribute type, such as DATA. A type with no name shows its value
// in hex, such as 0x1000.
std::wstring AttrTypeName(NTFS_BROWSER_NAMESPACE::Attr::Type type);

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
    NTFS_BROWSER_NAMESPACE::NtfsVolume<
        NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>& volume,
    std::wstring_view path);

}  // namespace NtfsAttrDump
