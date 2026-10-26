#include "io/entry-prober.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// First slot of $MFT that is read through $MFT's extents: below it, a record
// has a fixed address and is probed alone.
constexpr ULONGLONG first_mapped_slot = static_cast<ULONGLONG>(Mft::Idx::User);

// Widest hole, in bytes of $MFT, that one read spans between two probed
// records. 64 KiB is a block of the FullCache: two reads further apart than
// that never share a block, and a seek costs more than 64 KiB of transfer.
constexpr size_t max_gap_bytes = size_t{64} * 1024;

// Most bytes of $MFT one read takes, holes included. A group of one directory
// block rarely reaches it: it bounds a forged one.
constexpr size_t max_run_bytes = size_t{1} * 1024 * 1024;

// Records probed per round of announce, read and withdraw. The reader keeps at
// most 32 reads in flight and 1024 ranges announced: a round stays far below.
constexpr size_t max_chunk_records = 128;

// Records the memo holds before it is emptied: about 15 MiB. A directory
// beyond it probes again the records it forgot.
constexpr size_t max_known_records = size_t{1} << 18;

// One read of consecutive slots of $MFT, holes included.
struct Run {
  // The probed records the run serves, as indices of the sorted chunk.
  size_t begin;
  size_t end;
  ULONGLONG first_slot;
  ULONGLONG last_slot;
};

}  // namespace

template <Cache::Strategy S>
EntryProber<S>::EntryProber(const NtfsVolume<S>& volume)
    : volume_(&volume), named_(volume), levels_(Log::Detail::EnabledLevels()) {}

template <Cache::Strategy S>
void EntryProber<S>::ProbeGroup(std::span<const ULONGLONG> records) {
  if (known_.size() >= max_known_records) {
    known_.clear();
    logs_.clear();
  }

  pending_.clear();
  std::ranges::copy_if(
      records, std::back_inserter(pending_),
      [this](ULONGLONG record) { return !known_.contains(record); });
  std::ranges::sort(pending_);
  pending_.erase(std::ranges::unique(pending_).begin(), pending_.end());

  const std::span<const ULONGLONG> all(pending_);
  for (size_t first = 0; first < all.size(); first += max_chunk_records) {
    ProbeChunk(
        all.subspan(first, (std::min)(max_chunk_records, all.size() - first)));
  }
}

template <Cache::Strategy S>
NamedState EntryProber<S>::Take(ULONGLONG record) {
  auto known = known_.find(record);
  if (known == known_.end()) {
    ProbeChunk(std::span<const ULONGLONG>(&record, 1));
    known = known_.find(record);
  }

  if (known->second.logged) {
    const Logged& logged = logs_.at(record);
    logged.log.Replay(logged.before, logged.after);
  }
  return known->second.state;
}

template <Cache::Strategy S>
void EntryProber<S>::ProbeChunk(std::span<const ULONGLONG> chunk) {
  const DWORD record_size = volume_->GetFileRecordSize();
  const bool mapped = record_size != 0 && volume_->HasMftBase();

  // The records below first_mapped_slot sort first.
  size_t first_run_record = 0;
  while (first_run_record < chunk.size() &&
         (!mapped || chunk[first_run_record] < first_mapped_slot)) {
    first_run_record++;
  }

  std::vector<Run> runs;
  if (mapped) {
    const ULONGLONG max_gap_slots = max_gap_bytes / record_size;
    const ULONGLONG max_run_slots =
        (std::max<ULONGLONG>)(1, max_run_bytes / record_size);
    for (size_t index = first_run_record; index < chunk.size(); index++) {
      const ULONGLONG slot = chunk[index];
      if (!runs.empty()) {
        Run& run = runs.back();
        // The hole between the run and slot is narrow, and the run stays short.
        if (slot - run.last_slot - 1 <= max_gap_slots &&
            slot - run.first_slot < max_run_slots) {
          run.last_slot = slot;
          run.end = index + 1;
          continue;
        }
      }
      runs.push_back({.begin = index,
                      .end = index + 1,
                      .first_slot = slot,
                      .last_slot = slot});
    }
    // A forged slot makes the announcement log: the read of the record logs
    // it, at the entry.
    read_log_.Reset(levels_);
    const Log::Detail::CaptureScope scope(read_log_);
    for (const Run& run : runs) {
      volume_->PrefetchMftSlots(run.first_slot,
                                run.last_slot - run.first_slot + 1);
    }
  }

  for (size_t index = 0; index < first_run_record; index++) {
    ProbeOne(chunk[index], {});
  }
  for (const Run& run : runs) {
    run_bytes_.resize(gsl::narrow<size_t>(run.last_slot - run.first_slot + 1) *
                      record_size);
    bool read = false;
    {
      read_log_.Reset(levels_);
      const Log::Detail::CaptureScope scope(read_log_);
      read = volume_->ReadMftSlots(run.first_slot, run_bytes_);
    }
    for (size_t index = run.begin; index < run.end; index++) {
      const size_t offset =
          gsl::narrow<size_t>(chunk[index] - run.first_slot) * record_size;
      ProbeOne(
          chunk[index],
          read ? std::span<const BYTE>(run_bytes_).subspan(offset, record_size)
               : std::span<const BYTE>());
    }
  }

  // What nothing read, such as a block the cache already held, is withdrawn.
  if (!runs.empty()) {
    volume_->CancelPrefetch();
  }
}

template <Cache::Strategy S>
void EntryProber<S>::ProbeOne(ULONGLONG record, std::span<const BYTE> bytes) {
  Log::Detail::CapturedLog log;
  log.Reset(levels_);
  const Log::Detail::CapturedLog::Mark before = log.End();
  bool parsed = false;
  {
    const Log::Detail::CaptureScope scope(log);
    parsed = bytes.empty() ? named_.ParseFileRecord(record)
                           : named_.ParseFileRecord(record, bytes);
  }
  const Log::Detail::CapturedLog::Mark after = log.End();
  // The replay counts what the probe salvaged, at the entry.
  Log::Detail::ForgetRecoveredSince(before);

  // ParseFileRecord() judges a record with a failed update sequence check
  // twice when it reads the record itself, and once when it is given the
  // bytes: the log and the count would differ from the walk without batching.
  if (!bytes.empty() && named_.HasFailedFixup()) {
    ProbeOne(record, {});
    return;
  }

  Known known{
      .state = {.parsed = parsed,
                .deleted = parsed && named_.IsDeleted(),
                .sequence = parsed ? named_.GetSequenceNumber() : WORD{0}},
      .logged = false};
  if (after.lines != before.lines || after.recovered != before.recovered) {
    known.logged = true;
    logs_.insert_or_assign(
        record,
        Logged{.log = std::move(log), .before = before, .after = after});
  }
  known_.insert_or_assign(record, known);
}

template class EntryProber<Cache::Strategy::NoCache>;
template class EntryProber<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
