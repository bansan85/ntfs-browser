#include "usn/usn-journal-loader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "attr/resident.h"
#include "attr/std-info.h"
#include "log/ntfs-common.h"
#include "usn/usn-record-scanner.h"

namespace NTFS_BROWSER_NAMESPACE::Usn {

namespace {

// Name of the journal's file in $Extend.
constexpr std::wstring_view journal_file_name = L"$UsnJrnl";

// Name of the stream that holds the records.
constexpr std::wstring_view journal_stream_name = L"$J";

// Bytes asked of the stream at a time. 1 MiB is big enough to cost few calls
// for a journal of hundreds of MiB, and small enough to keep the scan's own
// buffer and the cut record it carries far below the cache ceiling.
constexpr size_t read_chunk_bytes = size_t{1} << 20U;

// Bits in a DWORD: where the high half of a FILETIME value starts.
constexpr unsigned dword_bits = 32;

// The records kept so far, newest last, and how many were dropped. A bounded
// ring: a new record pushes the oldest out once the limit is reached.
class Ring {
 public:
  explicit Ring(std::uint64_t limit)
      : limit_(static_cast<size_t>(std::min(
            limit,
            static_cast<std::uint64_t>(std::numeric_limits<size_t>::max())))) {}

  // Keeps records, or drops the oldest ones to make room.
  void Add(std::vector<Record>& records) {
    for (Record& record : records) {
      if (limit_ == 0) {
        truncated_ = true;
        continue;
      }
      if (kept_.size() == limit_) {
        kept_.pop_front();
        truncated_ = true;
      }
      kept_.push_back(std::move(record));
    }
    records.clear();
  }

  [[nodiscard]] bool Truncated() const noexcept { return truncated_; }

  // Moves the kept records out, oldest first.
  [[nodiscard]] std::vector<Record> Take() {
    std::vector<Record> out;
    out.reserve(kept_.size());
    for (Record& record : kept_) {
      out.push_back(std::move(record));
    }
    kept_.clear();
    return out;
  }

 private:
  size_t limit_;
  bool truncated_{false};
  std::deque<Record> kept_;
};

// A FILETIME the library gives out: the UTC value the journal holds, converted
// as every other timestamp of the library is.
void ConvertTime(Record& record) noexcept {
  const ULONGLONG utc =
      static_cast<ULONGLONG>(record.time.dwLowDateTime) |
      (static_cast<ULONGLONG>(record.time.dwHighDateTime) << dword_bits);
  Attr::AttrStdInfo<Attr::AttrResidentFullCache,
                    Cache::Strategy::FullCache>::UTC2Local(utc, record.time);
}

// What a read of the journal works on: the scanner that decodes it, the ring
// of records it keeps, and the journal that counts the bytes read.
struct ReadState {
  RecordScanner scanner;
  Ring ring;
  Journal journal;
};

// Reads one range of stream in chunks, through the scanner into the ring.
// Returns false when a read failed: that ends the range, the bytes before it
// were scanned, and the stretch that follows is lost.
template <Cache::Strategy S>
bool ReadRange(const Attr::AttrBase<S>& stream, const Alloc::ByteRange& range,
               ReadState& state) {
  state.scanner.Reset();
  std::vector<BYTE> buffer;
  std::vector<Record> batch;
  bool read_ok = true;
  ULONGLONG done = 0;
  while (done < range.length) {
    const auto want = static_cast<size_t>(
        std::min<ULONGLONG>(read_chunk_bytes, range.length - done));
    buffer.resize(want);
    const std::optional<ULONGLONG> got =
        stream.ReadData(range.offset + done, std::span<BYTE>(buffer));
    if (!got || *got != want) {
      read_ok = false;
      Log::Debug("USN journal: read failed at offset {}", range.offset + done);
      break;
    }
    state.scanner.Feed(buffer, range.offset + done, batch);
    state.ring.Add(batch);
    state.journal.scanned_bytes += want;
    done += want;
  }
  state.scanner.Finish(batch);
  state.ring.Add(batch);
  return read_ok;
}

// Finds $Extend\$UsnJrnl and reads its $J into journal. Null when a step
// fails. A step that throws is the caller's to catch.
template <Cache::Strategy S>
std::unique_ptr<Journal> ReadJournal(const NtfsVolume<S>& volume) {
  Io::FileRecord<S> extend(volume);
  extend.SetBypassDeletedGate(true);
  if (!extend.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Extend)) ||
      !extend.ParseAttrs()) {
    Log::Debug("USN journal: $Extend does not parse");
    return nullptr;
  }
  const std::optional<IndexEntry> entry =
      extend.FindSubEntry(journal_file_name);
  if (!entry) {
    Log::Debug("USN journal: $Extend has no $UsnJrnl");
    return nullptr;
  }

  Io::FileRecord<S> record(volume);
  record.SetBypassDeletedGate(true);
  record.SetAttrMask(Attr::Mask::Data);
  if (!record.ParseFileRecord(entry->GetFileReference()) ||
      !record.ParseAttrs()) {
    Log::Debug("USN journal: $UsnJrnl does not parse");
    return nullptr;
  }
  const Attr::AttrBase<S>* stream = record.FindStream(journal_stream_name);
  if (stream == nullptr) {
    Log::Debug("USN journal: $UsnJrnl has no $J stream");
    return nullptr;
  }

  const Alloc::StreamRanges ranges = stream->GetRealRanges();
  if (!ranges.complete) {
    Log::Info(
        "USN journal: the run list of $J is damaged, what it maps is read");
  }

  ReadState state{.scanner = {},
                  .ring = Ring(volume.GetOptions().max_usn_records),
                  .journal = {}};
  bool read_failed = false;
  for (const Alloc::ByteRange& range : ranges.ranges) {
    if (!ReadRange(*stream, range, state)) {
      read_failed = true;
    }
  }
  // Nothing at all could be read: the stream is unusable, not empty.
  if (read_failed && state.journal.scanned_bytes == 0) {
    return nullptr;
  }

  auto journal = std::make_unique<Journal>(std::move(state.journal));
  journal->records = state.ring.Take();
  journal->truncated = state.ring.Truncated();
  journal->damaged_records = state.scanner.DamagedRecords();
  journal->skipped_records = state.scanner.SkippedRecords();
  for (Record& item : journal->records) {
    ConvertTime(item);
  }
  return journal;
}

}  // namespace

template <Cache::Strategy S>
std::unique_ptr<Journal> LoadJournal(const NtfsVolume<S>& volume) {
  if (!volume.GetOptions().recover_errors) {
    return nullptr;
  }
  try {
    return ReadJournal(volume);
  } catch (const std::exception& error) {
    Log::Exception(error);
    return nullptr;
  }
}

// Each explicit instantiation of a function template needs the macro again, or
// the unit tests cannot link against it on a shared build.
template NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<Journal>
    LoadJournal<Cache::Strategy::NoCache>(
        const NtfsVolume<Cache::Strategy::NoCache>& volume);
template NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<Journal>
    LoadJournal<Cache::Strategy::FullCache>(
        const NtfsVolume<Cache::Strategy::FullCache>& volume);

}  // namespace NTFS_BROWSER_NAMESPACE::Usn
