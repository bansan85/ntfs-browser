#pragma once

// Shared body of the NtfsDir and NtfsDir2 samples. The two differ only in the
// cache strategy of the volume they open, so RunNtfsDir is instantiated once
// per strategy in ntfsdir-core.cpp.

#include <optional>
#include <string>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

// Prints command-line usage help.
void Usage();

// Splits the drive letter off a path such as "c:\windows". Returns the
// letter, or L'\0' if there is none. On success *ppath moves past the
// letter and the separators after it.
wchar_t Getvolume(wchar_t** ppath);

// Splits off and returns the first path segment of ppath (up to the next
// '\' or '"'), and removes that segment from ppath in place.
std::wstring Getpathname(std::wstring& ppath);

// Totals of the entries a listing shows.
struct Total {
  int files = 0;
  int dirs = 0;
};

// The parsed command line: the --log settings, whether to list every
// subdirectory, the volume options (--deleted, --recover), and the one path
// argument.
struct Arguments {
  NTFS_BROWSER_NAMESPACE::Log::Config log_config;
  bool recursive = false;
  NTFS_BROWSER_NAMESPACE::VolumeOptions volume_options;
  wchar_t* path = nullptr;
};

// Returns nullopt for a malformed --log option, or unless exactly one path
// is given. The --log, --recursive (-r), --deleted and --recover options may
// come before or after the path.
std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[]);

// Lists the directory named on the command line, opening the volume with
// cache strategy S. The target is a device or image file followed by a
// directory ("/dev/sda1/Windows", "c:\image.img\Windows"), or on Windows a
// drive letter ("c:\windows"). With --recursive, every directory below it is
// listed too, each under a "Directory of" header. With --deleted, a deleted
// entry is marked with * (and ? when its record cannot be read); with
// --recover, damaged directory data is salvaged. Returns the exit code.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
int RunNtfsDir(int argc, wchar_t* argv[]);
