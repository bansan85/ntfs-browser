#pragma once

// The directory walk of NtfsDir before it was built on Search::Scan. It opens
// a volume through NtfsVolume<S> and lists a folder, or a whole tree, with
// the index entries of each directory. NtfsScanBench runs it as its nocache
// and fullcache engines. The read ahead tests use it as an oracle: they list
// a fake volume that they hold and count the reads of, which Search::Scan does
// not allow. The text it writes and the reads it asks for are what their
// expectations were made from.

#include <cstdio>
#include <string>

#include <ntfs-browser/namespace.h>

#include "cache/strategy.h"
#include "ntfs-volume.h"
#include "volume-options.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DirWalk {

// What a listing names: the directory and the headers that show it.
struct ListTarget {
  // The volume in messages.
  std::wstring label;
  // The directory to list, relative to the root of the volume.
  std::wstring path;
  // The directory in the headers of a recursive listing.
  std::wstring root;
};

// Totals of the entries a listing shows.
struct Total {
  int files = 0;
  int dirs = 0;
};

// Splits the drive letter off a path such as "c:\windows". Returns the
// letter, or L'\0' if there is none. On success *ppath moves past the letter
// and the separators after it.
wchar_t Getvolume(wchar_t** ppath);

// Lists the directory target.path on an open volume, to stream (stdout, or a
// binary file a test reads back). With recursive, every directory below it is
// listed too, each under a "Directory of" header. The volume's tuning decides
// whether the next directories of a recursive listing are read ahead. The
// walk runs in the Bench::Phase::Walk phase. When total is not null, it gets
// the totals of the listing. Returns 0, or -1 after a message when the
// directory cannot be entered.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
int ListFrom(NTFS_BROWSER_NAMESPACE::NtfsVolume<S>& volume,
             const ListTarget& target, bool recursive,
             std::FILE* stream = stdout, Total* total = nullptr);

// Opens the volume that path names with cache strategy S, and lists the
// directory it leads to. The path is a device or image file followed by a
// directory ("/dev/sda1/Windows", "c:\image.img\Windows"), or on Windows a
// drive letter ("c:\windows"). The options are the volume options of the
// listing: include_deleted marks the deleted entries with *, and
// recover_errors salvages damaged directory data. Messages go to stdout.
// Returns 0, or -1 after a message when the volume or the directory cannot be
// opened.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
int ListDirectory(wchar_t* path, bool recursive,
                  const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                  std::FILE* stream = stdout, Total* total = nullptr);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DirWalk
