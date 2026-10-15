#pragma once

// Dump of a file's first bytes, behind the ntfsdump dialog. Plain C++ over the
// library, with no MFC types, so the unit tests can drive it without a window.

#include <string>
#include <string_view>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

namespace NtfsDump {

// What one dump produced. message is empty on success. Otherwise it is the
// text the dialog shows, and dump stays empty.
struct DumpResult {
  std::wstring message;
  std::wstring dump;
};

// Dumps the start of the unnamed $DATA stream of the file at path, a full path
// such as C:\dir\file. A file with no unnamed stream dumps as empty text.
DumpResult
    DumpFile(NTFS_BROWSER_NAMESPACE::NtfsVolume<
                 NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>& volume,
             std::wstring_view path);

}  // namespace NtfsDump
