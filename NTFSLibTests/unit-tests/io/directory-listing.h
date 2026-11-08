#pragma once

// The listing walk of NtfsDir before it was built on Search::Scan, kept as a
// test oracle: the read ahead tests list a fake volume that they hold and
// count the reads of, which Search::Scan does not allow. The text it writes
// and the reads it asks for are what their expectations were made from.

#include <cstdio>
#include <string>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

namespace NTFS_BROWSER_NAMESPACE::Test::Io {

// What a listing names: the directory and the headers that show it.
struct ListTarget {
  // The volume in messages.
  std::wstring label;
  // The directory to list, relative to the root of the volume.
  std::wstring path;
  // The directory in the headers of a recursive listing.
  std::wstring root;
};

// Lists the directory target.path on an open volume, to stream (a binary file
// a test reads back). With recursive, every directory below it is listed too,
// each under a "Directory of" header. The volume's tuning decides whether the
// next directories of a recursive listing are read ahead. Returns 0, or -1
// after a message when the directory cannot be entered.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
int ListFrom(NTFS_BROWSER_NAMESPACE::NtfsVolume<S>& volume,
             const ListTarget& target, bool recursive, std::FILE* stream);

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Io
