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
#include <ntfs-browser/ntfs-volume.h>

namespace NtfsUndel {

// Most rows one search lists. The scan stops once this many have matched.
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
  // Path from the volume root, built from the directory entries the scan saw.
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
    NtfsBrowser::NtfsVolume<NtfsBrowser::Cache::Strategy::FullCache>& volume,
    std::wstring_view filter, const SearchControl& control);

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
  std::unique_ptr<
      NtfsBrowser::Io::FileRecord<NtfsBrowser::Cache::Strategy::NoCache>>
      record;
  RecoverError error = RecoverError::None;
};

// Parses the freed record of reference, with its $DATA attributes.
OpenedRecord OpenDeletedFile(
    NtfsBrowser::NtfsVolume<NtfsBrowser::Cache::Strategy::NoCache>& volume,
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
    const NtfsBrowser::Io::FileRecord<NtfsBrowser::Cache::Strategy::NoCache>&
        record,
    const std::function<bool(std::uint64_t size)>& confirm_large,
    const std::function<bool(std::span<const BYTE> chunk)>& write);

}  // namespace NtfsUndel
