#pragma once

#include <ntfs-browser/win-types.h>

#include <map>
#include <optional>
#include <string>

namespace NtfsCompare
{

// One file or directory's metadata, as one method reports it. A field left
// std::nullopt means this method cannot structurally provide it - not a
// disagreement with another method, which reports std::nullopt the same way.
struct Entry
{
  bool is_directory = false;
  std::optional<ULONGLONG> logical_size;
  std::optional<ULONGLONG> physical_size;
  // Every timestamp is normalized to UTC, in 100 ns ticks since 1601-01-01
  // (a Win32 FILETIME's own units), whatever unit or timezone the source API
  // used.
  std::optional<ULONGLONG> creation_time_utc;
  std::optional<ULONGLONG> modification_time_utc;
  std::optional<ULONGLONG> change_time_utc;
  std::optional<ULONGLONG> access_time_utc;
  std::optional<bool> read_only;
  std::optional<bool> hidden;
  std::optional<bool> system;
  std::optional<bool> archive;
  std::optional<bool> compressed;
  std::optional<bool> encrypted;
  std::optional<bool> sparse;
};

// One method's full listing of a directory tree, keyed by the entry's path
// relative to the directory given on the command line: "/"-separated, long
// name only (a DOS 8.3 alias is never a key on its own).
using Listing = std::map<std::wstring, Entry>;

}  // namespace NtfsCompare
