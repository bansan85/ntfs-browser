#pragma once

// Search and recovery behind the ntfsundel command. Plain C++ over Search::Scan
// and Search::Reader, so the unit tests drive it without a process. Builds on
// Windows and POSIX.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/search/types.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel {

// Most rows one search lists. The scan stops once this many have matched. A
// bound on the size of a listing, not an NTFS limit, so one search stays
// bounded on a busy volume.
inline constexpr std::size_t kMaxListedFiles = 5000;

// Recovering a file this large needs a yes first. Likely a damaged size.
inline constexpr std::uint64_t kConfirmFileSize = 100 * 1024 * 1024;

// One deleted file that a scan listed.
struct DeletedFile {
  // The stable name of the file: a Reader reads the file from it.
  NTFS_BROWSER_NAMESPACE::Search::Handle handle;
  // MFT record number of the freed record.
  std::uint64_t reference = 0;
  // Name the record carries.
  std::wstring name;
  // Path from the volume root, such as \Docs\old.tmp. A file that no
  // directory reaches starts with \<lost>.
  std::wstring full_path;
  // Last write time in UTC, as 100 ns ticks since 1601.
  std::uint64_t write_time = 0;
};

// Hooks that let the caller keep the scan stoppable.
struct SearchControl {
  // Rows the search lists at most. Reaching it ends the scan and sets
  // SearchResult::truncated.
  std::size_t max_listed = kMaxListedFiles;
  // Polled about every 100 ms. True ends the scan.
  std::function<bool()> stop_requested;
};

struct SearchResult {
  // The deleted files, one for each record, in record number order.
  std::vector<DeletedFile> files;
  // True when the list stopped at SearchControl::max_listed.
  bool truncated = false;
};

// Lists the deleted files of target with a Search::Scan: the MftList engine,
// with the path of each file. target is a drive, an image file or a device, and
// an optional folder inside the volume, which restricts the list to the files
// below it. recover_errors is the tolerance of the volume. pattern, when set,
// keeps only the files whose name or path matches. Returns nullopt when the
// scan could not run: the target does not open, or is no NTFS volume.
std::optional<SearchResult> ListDeletedFiles(
    std::wstring_view target, bool recover_errors, const SearchControl& control,
    const std::optional<NTFS_BROWSER_NAMESPACE::Search::Pattern>& pattern = {});

// The record numbers of the deleted files whose name matches wildcard, as the
// --name option reads it: '*' and '?', without regard to case, letters of any
// script folded through the $UpCase table of the volume (which stays inside
// the library). A wildcard with a '' or '/' in it is matched against the whole
// path, from the root when it starts with one, else from any folder. Nothing
// when the scan could not run.
std::optional<std::set<std::uint64_t>>
    FindNamedFiles(std::wstring_view target, bool recover_errors,
                   std::wstring_view wildcard);

// The Search::Pattern that FindNamedFiles() uses for wildcard.
[[nodiscard]] NTFS_BROWSER_NAMESPACE::Search::Pattern
    MakeNamePattern(std::wstring_view wildcard);

// The text as UTF-16 units, the way a Search::Options target takes it.
[[nodiscard]] std::u16string ToUtf16(std::wstring_view text);

// Warning to show when a search stops at kMaxListedFiles. Built from
// that constant, so the number shown is the limit the scan enforces.
std::wstring TooManyFilesMessage();

// Formats UTC ticks as "YYYY-MM-DD  HH:MM" in the local zone of the host.
// Zeros when the host cannot convert the time.
std::wstring FormatLocalTime(std::uint64_t utc_ticks);

enum class RecoverError : std::uint8_t {
  // Recovery went through.
  None,
  // The record cannot be read: it does not parse, or it is another file's now.
  Unreadable,
  // The record has no unnamed $DATA stream.
  NoDataStream,
  // The caller refused a large file.
  Declined,
  // A read came back short or failed.
  Read,
  // The caller's write failed.
  Write
};

struct CopyResult {
  RecoverError error = RecoverError::None;
  // Bytes handed to the writer, when error is None.
  std::uint64_t bytes = 0;
};

// Hands the unnamed $DATA stream of the file that handle names to write, chunk
// by chunk, in order. confirm_large is asked once before a file over
// kConfirmFileSize; false declines. write returns false to abort the copy.
CopyResult CopyUnnamedData(
    NTFS_BROWSER_NAMESPACE::Search::Reader& reader,
    const NTFS_BROWSER_NAMESPACE::Search::Handle& handle,
    const std::function<bool(std::uint64_t size)>& confirm_large,
    const std::function<bool(std::span<const std::uint8_t> chunk)>& write);

// What to do before copying a deleted file.
enum class RecoveryAdvice : std::uint8_t {
  // Copy with no question.
  Proceed,
  // Ask first: the bytes are probably wrong.
  Confirm,
  // Copy, and tell the user that the state could not be checked.
  Warn
};

// Why a recovery is not plain.
enum class RecoveryCause : std::uint8_t {
  // Nothing to say.
  None,
  // The clusters were given to another file after the deletion.
  Reallocated,
  // The clusters are marked bad in $BadClus.
  BadClusters,
  // The allocation could not be read.
  Unchecked
};

struct RecoveryCheck {
  RecoveryAdvice advice = RecoveryAdvice::Proceed;
  RecoveryCause cause = RecoveryCause::None;
};

// Turns the data state of the unnamed $DATA stream into advice: intact goes
// ahead, reallocated or bad asks first, and an unknown state warns. NotChecked
// goes ahead: the copy reports a missing stream itself.
RecoveryCheck AssessRecovery(NTFS_BROWSER_NAMESPACE::Search::DataState state);

// The data state of the unnamed stream of handle, as advice. A file with no
// unnamed stream goes ahead.
RecoveryCheck
    AssessRecovery(NTFS_BROWSER_NAMESPACE::Search::Reader& reader,
                   const NTFS_BROWSER_NAMESPACE::Search::Handle& handle);

// The text that names check.cause, a statement with no question in it. Empty
// for None.
std::wstring RecoveryMessage(const RecoveryCheck& check);

// True when the copy may go on. A Proceed check is true without asking. Any
// other check is put to confirm, whose answer is returned: the caller shows
// the message and answers false to stop. The caller copies nothing when this
// is false.
bool ShouldRecover(const RecoveryCheck& check,
                   const std::function<bool(const RecoveryCheck&)>& confirm);

// Longest name a file system component holds, in UTF-16 units.
inline constexpr std::size_t kMaxFileNameUnits = 255;

// Longest path Win32 takes, in UTF-16 units, with the extended-length prefix
// and the terminator. A longer one cannot be created. The same bound holds on
// POSIX, so a recovery path has one limit on every platform.
inline constexpr std::size_t kMaxExtendedPathUnits = 32767;

// Copies of a name tried when it is taken, before the recovery gives up:
// "name (2)" to "name (9999)".
inline constexpr unsigned kMaxNameCopies = 9999;

// Turns the name of a recovered file into one Win32 can create and every tool
// can open again. NTFS keeps names that Win32 refuses or rewrites: a trailing
// dot or space (dropped), a character of "<>:\"/\\|?*" or below U+0020 (each
// becomes '_'), a device name such as CON, NUL, COM1 or LPT9, with or without
// an extension (gets a leading '_'), and an empty name (becomes "_"). A name
// over kMaxFileNameUnits is cut there, never inside a surrogate pair. A name
// that needs none of this is returned as it is. An unpaired surrogate is kept:
// Win32 accepts it.
std::wstring SafeFileName(std::wstring_view name);

// Why the file a recovery created does not carry the name it was given.
enum class NameChange : std::uint8_t {
  // The name is as asked.
  None,
  // SafeFileName() changed it: Win32 cannot create the name the record has.
  Unsafe,
  // The folder held a file of that name, or one that differs only by case:
  // a copy number was added, and nothing was overwritten.
  Taken
};

// What to do when the folder already holds a file of the name.
enum class ExistingFile : std::uint8_t {
  // Overwrite it. The caller has asked the user.
  Replace,
  // Keep it, and create "name (2)", "name (3)" and so on.
  KeepBoth
};

// Why a recovery file cannot be created.
enum class CreateError : std::uint8_t {
  // The path is longer than Win32 takes, even with the extended-length prefix.
  PathTooLong,
  // The folder is missing, or cannot be written, or the system refused.
  Failed,
  // Every copy of the name up to kMaxNameCopies is taken.
  NoFreeName
};

// The handle of the operating system for a file: a Win32 HANDLE or a POSIX file
// descriptor, held as an integer.
using NativeFileHandle = std::intptr_t;

// A handle that names no file. It is also what Win32 returns for a failure.
inline constexpr NativeFileHandle no_file_handle = -1;

// A file a recovery writes into. It closes when it goes.
class RecoveryFile {
 public:
  RecoveryFile(NativeFileHandle handle, std::wstring path, NameChange change);
  RecoveryFile(const RecoveryFile&) = delete;
  RecoveryFile& operator=(const RecoveryFile&) = delete;
  RecoveryFile(RecoveryFile&& other) noexcept;
  RecoveryFile& operator=(RecoveryFile&& other) noexcept;
  ~RecoveryFile();

  // Writes all of chunk. A failed or partial write is a failure.
  [[nodiscard]] bool Write(std::span<const std::uint8_t> chunk);

  // The path of the file as a user types it: the folder and the final name,
  // without the extended-length prefix.
  [[nodiscard]] const std::wstring& Path() const noexcept;

  // Whether the final name is the one that was asked for.
  [[nodiscard]] NameChange Change() const noexcept;

 private:
  void Close() noexcept;

  // The handle of the open file, no_file_handle once closed.
  NativeFileHandle handle_;
  std::wstring path_;
  NameChange change_;
};

// What CreateRecoveryFile() made, or why it could not.
struct CreatedFile {
  std::optional<RecoveryFile> file;
  CreateError error = CreateError::Failed;
};

// Creates the file a recovery writes into, named name, in the folder
// directory. The name goes through SafeFileName() first, so a file made on
// POSIX opens on Windows too. On POSIX the name is also cut to 240 bytes of
// UTF-8, which leaves room for a copy number under the 255 bytes a file system
// takes. The path goes through the extended-length form (see
// ToExtendedLengthPath()), so a path of up to 32 767 units works on Windows
// whatever the LongPathsEnabled switch says. directory is a path as
// WideFromPath() writes it. With
// ExistingFile::KeepBoth, a file that exists is never overwritten. Names that
// differ only by case are the same name in a folder Win32 reads without regard
// to case, so two files recovered as "readme" and "README" end as "readme" and
// "README (2)". The file reports what it did through RecoveryFile::Change().
CreatedFile CreateRecoveryFile(std::wstring_view directory,
                               std::wstring_view name, ExistingFile existing);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel
