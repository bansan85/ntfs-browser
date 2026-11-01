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
#include <ntfs-browser/tuning/tuning.h>

#include "bench/recorder.h"
#include "io/mft-runs.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// First slot of $MFT that is read through $MFT's extents: below it, a record
// has a fixed address and is probed alone.
constexpr ULONGLONG first_mapped_slot = static_cast<ULONGLONG>(Mft::Idx::User);

}  // namespace

template <Cache::Strategy S>
EntryProber<S>::EntryProber(const NtfsVolume<S>& volume,
                            const typename FileRecord<S>::ProbeStore* store)
    : volume_(&volume),
      store_(store),
      named_(volume),
      levels_(Log::Detail::EnabledLevels()),
      tuning_(volume.GetOptions().tuning.probe) {}

template <Cache::Strategy S>
void EntryProber<S>::ProbeGroup(std::span<const ULONGLONG> records) {
  volume_->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  if (known_.size() >= tuning_.max_known_records) {
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
  const size_t chunk_records = (std::max<size_t>)(1, tuning_.max_chunk_records);
  for (size_t first = 0; first < all.size(); first += chunk_records) {
    ProbeChunk(
        all.subspan(first, (std::min)(chunk_records, all.size() - first)));
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

  // The records that the store gives are probed from its bytes, and the others
  // are read.
  std::span<const ULONGLONG> to_read = chunk.subspan(first_run_record);
  if (mapped && store_ != nullptr && store_->find) {
    missing_.clear();
    for (const ULONGLONG record : to_read) {
      const std::span<const BYTE> held = store_->find(record);
      if (held.size() == record_size) {
        ProbeOne(record, held);
      } else {
        missing_.push_back(record);
      }
    }
    to_read = missing_;
  }

  std::vector<MftRun> runs;
  if (mapped) {
    runs = PlanMftRuns(to_read, tuning_.max_gap_bytes / record_size,
                       tuning_.max_run_bytes / record_size);
    // A run that is kept is kept whole: the disk reads its clusters anyway.
    if (store_ != nullptr && store_->keep) {
      runs = WidenToClusters(std::move(runs),
                             volume_->GetClusterSize() / record_size,
                             first_mapped_slot);
    }
    // A forged slot makes the announcement log: the read of the record logs
    // it, at the entry.
    read_log_.Reset(levels_);
    const Log::Detail::CaptureScope scope(read_log_);
    for (const MftRun& run : runs) {
      volume_->PrefetchMftSlots(run.first_slot,
                                run.last_slot - run.first_slot + 1);
    }
  }

  for (size_t index = 0; index < first_run_record; index++) {
    ProbeOne(chunk[index], {});
  }
  for (const MftRun& run : runs) {
    run_bytes_.resize(gsl::narrow<size_t>(run.last_slot - run.first_slot + 1) *
                      record_size);
    bool read = false;
    {
      read_log_.Reset(levels_);
      const Log::Detail::CaptureScope scope(read_log_);
      const Bench::Detail::KindScope kind(Bench::ReadKind::ProbeRun);
      read = volume_->ReadMftSlots(run.first_slot, run_bytes_);
    }
    if (read && store_ != nullptr && store_->keep) {
      store_->keep(run.first_slot, run_bytes_);
    }
    for (size_t index = run.begin; index < run.end; index++) {
      const size_t offset =
          gsl::narrow<size_t>(to_read[index] - run.first_slot) * record_size;
      ProbeOne(
          to_read[index],
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
