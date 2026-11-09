#pragma once

// Dump of a file's first bytes, behind the ntfsdump command. Plain C++ over the
// library, so main.cpp only hands over argv and the unit tests drive it without
// a process.

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/volume-target.h>

#include "cache/strategy.h"
#include "ntfs-volume.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDump {

// What one dump produced. message is empty on success. Otherwise it is the
// error to report, and dump stays empty.
struct DumpResult {
  std::wstring message;
  std::wstring dump;
};

// Dumps the start of the unnamed $DATA stream of the file at path, a path
// inside the volume such as dir\file. Every row is an address, 16 bytes in hex
// and a text column, and ends in LF. The hex of a short last row is blank where
// bytes are missing, and the text column shows a byte outside printable ASCII
// as '.'. A file with no unnamed stream dumps as empty text.
DumpResult
    DumpFile(NTFS_BROWSER_NAMESPACE::NtfsVolume<
                 NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>& volume,
             std::wstring_view path);

// Exit code of a run that fails.
inline constexpr int failure_exit_code = 1;

// The parsed command line: the options every volume tool takes, and the one
// target.
struct Arguments {
  NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::CommonArguments common;
  std::wstring target;
};

// The text --help prints.
std::string UsageText();

// Returns nullopt for a malformed --log option, or unless exactly one target
// is given. --help needs no target. The options may come before or after it.
std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[]);

// Opens the volume that args.target names and writes the dump to out as
// UTF-8. A failure goes to err as a line of text. Returns the exit code.
int DumpTarget(const Arguments& args, std::FILE* out, std::FILE* err);

// Parses argv, then dumps. Usage goes to stdout for --help and to stderr for
// a bad command line. Returns the exit code.
int RunNtfsDump(int argc, wchar_t* argv[]);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDump
