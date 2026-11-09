#pragma once

// The ntfsundel command: lists the deleted files of a volume and recovers
// some of them into a folder. Plain C++ over ntfsundel-core.h, so main.cpp only
// hands over argv and the unit tests drive it without a process.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/example/ntfsundel/ntfsundel-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader.h>
#include <ntfs-browser/tool/device-access/volume-target.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel {

// Exit code of a run that fails.
inline constexpr int failure_exit_code = 1;

// The parsed command line.
struct Arguments {
  NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::CommonArguments common;
  // A drive path, an image file or a device, then the folder inside the volume
  // that restricts the listing. Empty means the whole volume.
  std::wstring target;
  // --all: select every listed file.
  bool all = false;
  // --record N, once per record: select the listed file of that MFT record.
  std::vector<std::uint64_t> records;
  // --name PATTERN, once per pattern: select the listed files whose name
  // matches. See FindNamedFiles().
  std::vector<std::wstring> names;
  // --output DIR: the folder the recovered files go into. Empty means list
  // only.
  std::wstring output;
  // --include-doubtful: also recover a file whose data is probably another
  // file's, sits on bad clusters, or is over kConfirmFileSize.
  bool include_doubtful = false;

  // Reports whether the command line selects files by itself: --all, --record
  // or --name.
  [[nodiscard]] bool HasSelection() const {
    return all || !records.empty() || !names.empty();
  }
};

// The text --help prints.
std::string UsageText();

// Returns nullopt for a command line that cannot run, and says why in error.
// --help needs no target. Otherwise exactly one target is needed. An option
// that takes a value reads it from the next argument or after an equal sign:
// "--output dir" and "--output=dir".
std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[],
                                        std::wstring& error);

// What SelectFiles() chose.
struct Selection {
  // Positions in the list, in list order, each once.
  std::vector<std::size_t> positions;
  // The --record values that no listed file carries.
  std::vector<std::uint64_t> missing_records;
};

// Applies --all, --record and --name of args to files. A file is chosen when
// any of them picks it. named holds the record numbers that the --name patterns
// found: the volume decides when two names are the same, so the scan does the
// matching, see FindNamedFiles().
[[nodiscard]] Selection SelectFiles(const std::vector<DeletedFile>& files,
                                    const Arguments& args,
                                    const std::set<std::uint64_t>& named);

// Reads a list of numbers a user typed: "1,3 5-8" or "all". count is the
// number of files listed. Returns the positions, from 0, in the order typed
// with no repeat. Returns nullopt when a number is 0, past count, or not a
// number, or when a range runs backwards. An empty line gives an empty list.
[[nodiscard]] std::optional<std::vector<std::size_t>>
    ParseNumberList(std::string_view line, std::size_t count);

// What is known of a freed file before it is recovered.
struct FileState {
  // False when the record does not open. The rest is then unset.
  bool readable = false;
  // Whether the record has an unnamed $DATA stream.
  bool has_data = false;
  // Bytes of that stream.
  std::uint64_t size = 0;
  // What AssessRecovery() says of the clusters.
  RecoveryCheck check;

  // Reports whether recovering needs --include-doubtful: the data is probably
  // wrong, or the size is over kConfirmFileSize.
  [[nodiscard]] bool IsDoubtful() const {
    return check.advice == RecoveryAdvice::Confirm || size > kConfirmFileSize;
  }
};

// Asks reader for the state of the deleted file. Reads no data.
[[nodiscard]] FileState DescribeFile(NTFS_BROWSER_NAMESPACE::Reader& reader,
                                     const DeletedFile& file);

// The word a listing shows for state: "intact", "reallocated", "bad",
// "unchecked", "no-data" or "unreadable".
[[nodiscard]] std::wstring_view StateName(const FileState& state);

// One line of the listing, with no line end: the number from 1, the MFT record,
// the size in bytes, the state, the last write time and the path.
[[nodiscard]] std::wstring FormatListLine(std::size_t number,
                                          const DeletedFile& file,
                                          const FileState& state);

// Says which volume of the host holds a path. See HostVolumeId().
using VolumeIdFunction =
    std::function<std::optional<std::string>(const std::filesystem::path&)>;

// Returns an empty text when output may receive recovered files, and the
// reason when it may not. A folder on the volume that source is read from is
// refused: the files would be written over the data of the deleted ones. A
// volume that cannot be told is refused too. source is the image file, the
// device or the drive root the volume is read from.
[[nodiscard]] std::wstring
    CheckOutputVolume(const std::filesystem::path& source,
                      const std::filesystem::path& output,
                      const VolumeIdFunction& volume_id);

// What a run talks to.
struct Environment {
  // The list, the prompts and the recovery report go here.
  std::FILE* out = nullptr;
  // Errors and warnings go here.
  std::FILE* err = nullptr;
  // Where the interactive mode reads the numbers.
  std::istream* in = nullptr;
  // Whether the user can answer: input is a terminal.
  bool interactive = false;
  // The volume identity of a path.
  VolumeIdFunction volume_id;
};

// Lists the deleted files of args.target. With args.output, recovers the ones
// selected there too. Returns the exit code.
int UndelTarget(const Arguments& args, const Environment& environment);

// Parses argv, then runs. Usage goes to stdout for --help and to stderr for a
// bad command line. Returns the exit code.
int RunNtfsUndel(int argc, wchar_t* argv[]);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel
