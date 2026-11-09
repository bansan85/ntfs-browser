#pragma once

// Attribute dump behind the ntfsattr command. Plain C++ over the library, so
// main.cpp only hands over argv and the unit tests drive it without a process.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/volume-target.h>

#include "attr/type.h"
#include "cache/strategy.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsAttr {

// Appends a hex dump of datalen bytes at data to lines, 16 bytes a row. Every
// row ends in LF. The hex of a short last row is blank where bytes are missing,
// and the text column shows a byte outside printable ASCII as '.'. Zero bytes
// append nothing, and data is not read.
void AppendData(std::wstring& lines, const BYTE* data, std::uint32_t datalen);

// Name of an attribute type, such as DATA. A type with no name shows its value
// in hex, such as 0x1000.
std::wstring AttrTypeName(NTFS_BROWSER_NAMESPACE::Attr::Type type);

// The path a dump works on for the picked path inside a volume. With directory
// set, that is the folder holding it. The root gives the empty path.
std::wstring TargetPath(std::wstring_view picked, bool directory);

// What one dump produced. message is empty on success. Otherwise it is the
// error to report, and dump stays empty.
struct DumpResult {
  std::wstring message;
  std::wstring dump;
};

// Dumps the raw header of every attribute of the file or directory at path, a
// path inside the volume such as dir\file. The empty path dumps the root
// directory. Each attribute opens with a heading line.
DumpResult DumpAttributes(
    NTFS_BROWSER_NAMESPACE::NtfsVolume<
        NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>& volume,
    std::wstring_view path);

// Exit code of a run that fails.
inline constexpr int failure_exit_code = 1;

// The parsed command line: the options every volume tool takes, whether to
// dump the folder that holds the target, and the one target.
struct Arguments {
  NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::CommonArguments common;
  bool directory = false;
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
int RunNtfsAttr(int argc, wchar_t* argv[]);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsAttr
