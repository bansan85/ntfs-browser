#pragma once

// Search and recovery behind the ntfsundel dialog. Plain C++ over the library,
// with no MFC types, so the unit tests can drive it without a window.

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

namespace NtfsUndel {

// Most rows one search lists. The scan stops once this many have matched. A UI
// bound, not an NTFS limit, so one search stays bounded on a busy volume.
inline constexpr std::size_t kMaxListedFiles = 5000;

// Bytes per read and write while recovering a file. Keeps memory bounded for
// files of any size.
inline constexpr std::uint32_t kCopyChunkSize = 64 * 1024;

// Recovering a file this large asks the user first. Likely a damaged size.
inline constexpr std::uint64_t kConfirmFileSize = 100 * 1024 * 1024;

// One freed file whose name matched the search.
struct DeletedFile {
  // MFT record number of the freed record.
  std::uint64_t reference = 0;
  // Name the record carries.
  std::wstring name;
  // Path from the volume root, such as \Docs\old.tmp, built from the directory
  // entries the scan saw. Only the name when no entry links it to the root.
  std::wstring full_path;
  // Last write time in local time, as FILETIME ticks: 100 ns since 1601.
  std::uint64_t write_time = 0;
};

// Hooks that let the caller keep the scan responsive and stoppable.
struct SearchControl {
  // Record numbers at or past this value are not scanned.
  std::uint64_t max_records = 500000;
  // Rows the search lists at most. Reaching it ends the scan and sets
  // SearchResult::truncated.
  std::size_t max_listed = kMaxListedFiles;
  // Polled before every record. True ends the scan.
  std::function<bool()> stop_requested;
  // Polled every few thousand records, at most every 100 ms. False ends the
  // scan.
  std::function<bool()> pump_messages;
};

struct SearchResult {
  // Matching freed files, in record number order.
  std::vector<DeletedFile> files;
  // True when the list stopped at SearchControl::max_listed.
  bool truncated = false;
  // Duration of the record scan alone, in microseconds.
  std::int64_t scan_us = 0;
};

// Lists the freed files of volume whose name matches filter, a whole-name
// ECMAScript regex. Leading and trailing blanks are dropped, and an empty
// filter matches every name. Returns nullopt when the filter is not a valid
// regex. The volume must be opened with include_deleted set.
std::optional<SearchResult> SearchDeletedFiles(
    NTFS_BROWSER_NAMESPACE::NtfsVolume<
        NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>& volume,
    std::wstring_view filter, const SearchControl& control);

// Warning the dialog shows when a search stops at kMaxListedFiles. Built from
// that constant, so the number shown is the limit the scan enforces.
std::wstring TooManyFilesMessage();

// Formats FILETIME ticks as "YYYY-MM-DD  HH:MM", with no zone conversion.
// Zeros when the time is out of the SYSTEMTIME range.
std::wstring FormatWriteTime(std::uint64_t ticks);

enum class RecoverError : std::uint8_t {
  // Recovery went through.
  None,
  // The record does not parse.
  RecordParse,
  // The record's attributes do not parse.
  AttributeParse,
  // The record has no unnamed $DATA stream.
  NoDataStream,
  // The caller refused a large file.
  Declined,
  // A read came back short or failed.
  Read,
  // The caller's write failed.
  Write
};

// A parsed freed record. The record owns the attributes read later.
struct OpenedRecord {
  // Null unless error is None.
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::Io::FileRecord<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>>
      record;
  RecoverError error = RecoverError::None;
};

// Parses the freed record of reference, with its $DATA attributes.
OpenedRecord OpenDeletedFile(
    NTFS_BROWSER_NAMESPACE::NtfsVolume<
        NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>& volume,
    std::uint64_t reference);

struct CopyResult {
  RecoverError error = RecoverError::None;
  // Bytes handed to the writer, when error is None.
  std::uint64_t bytes = 0;
};

// Hands the unnamed $DATA stream of record to write, chunk by chunk, in order.
// confirm_large is asked once before a file over kConfirmFileSize; false
// declines. write returns false to abort the copy.
CopyResult CopyUnnamedData(
    const NTFS_BROWSER_NAMESPACE::Io::FileRecord<
        NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>& record,
    const std::function<bool(std::uint64_t size)>& confirm_large,
    const std::function<bool(std::span<const BYTE> chunk)>& write);

// What to do before copying a freed file.
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

// Asks the volume for the data state of the unnamed $DATA stream of record and
// turns it into advice: intact goes ahead, reallocated or bad asks first, and
// an unknown state warns. Reads no data. A record with no unnamed stream goes
// ahead: the copy reports the missing stream itself.
RecoveryCheck
    AssessRecovery(const NTFS_BROWSER_NAMESPACE::Io::FileRecord<
                   NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>& record);

// The text that names check.cause, for a dialog. Empty for None.
std::wstring RecoveryMessage(const RecoveryCheck& check);

// True when the copy may go on. A Proceed check is true without asking. Any
// other check is put to confirm, whose answer is returned: the caller shows
// the message and answers false to stop. The caller copies nothing when this
// is false. Uses no window and no Win32 call, so a command-line tool can reuse
// it.
bool ShouldRecover(
    const NTFS_BROWSER_NAMESPACE::Io::FileRecord<
        NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>& record,
    const std::function<bool(const RecoveryCheck&)>& confirm);

// Longest name a file system component holds, in UTF-16 units.
inline constexpr std::size_t kMaxFileNameUnits = 255;

// Longest path Win32 takes, in UTF-16 units, with the extended-length prefix
// and the terminator. A longer one cannot be created.
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

// A file a recovery writes into. It closes when it goes.
class RecoveryFile {
 public:
  RecoveryFile(void* handle, std::wstring path, NameChange change);
  RecoveryFile(const RecoveryFile&) = delete;
  RecoveryFile& operator=(const RecoveryFile&) = delete;
  RecoveryFile(RecoveryFile&& other) noexcept;
  RecoveryFile& operator=(RecoveryFile&& other) noexcept;
  ~RecoveryFile();

  // Writes all of chunk. A failed or partial write is a failure.
  [[nodiscard]] bool Write(std::span<const BYTE> chunk);

  // The path of the file as a user types it: the folder and the final name,
  // without the extended-length prefix.
  [[nodiscard]] const std::wstring& Path() const noexcept;

  // Whether the final name is the one that was asked for.
  [[nodiscard]] NameChange Change() const noexcept;

 private:
  void Close() noexcept;

  // The Win32 handle, null once closed.
  void* handle_;
  std::wstring path_;
  NameChange change_;
};

// What CreateRecoveryFile() made, or why it could not.
struct CreatedFile {
  std::optional<RecoveryFile> file;
  CreateError error = CreateError::Failed;
};

// Creates the file a recovery writes into, named name, in the folder
// directory. The name goes through SafeFileName() first, and the path through
// the extended-length form (see ToExtendedLengthPath()), so a path of up to
// 32 767 units works whatever the LongPathsEnabled switch says. With
// ExistingFile::KeepBoth, a file that exists is never overwritten. Names that
// differ only by case are the same name in a folder Win32 reads without regard
// to case, so two files recovered as "readme" and "README" end as "readme" and
// "README (2)". The file reports what it did through RecoveryFile::Change().
CreatedFile CreateRecoveryFile(std::wstring_view directory,
                               std::wstring_view name, ExistingFile existing);

}  // namespace NtfsUndel
