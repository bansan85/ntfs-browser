#include "io/entry-prober.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "bench/bench.h"
#include "bench/recorder.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "io/mft-runs.h"
#include "log/ntfs-common.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

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
  const bool keeps = store_ != nullptr && store_->keep;
  const ProbeRunRules rules = ProbeRunRulesOf(*volume_, keeps);

  // The records that the store gives are probed from its bytes, and the others
  // are read.
  const HeldRecordSource none;
  const ProbeRunPlan plan =
      PlanProbeRuns(chunk, rules, store_ != nullptr ? store_->find : none,
                    [this](ULONGLONG record, std::span<const BYTE> held) {
                      ProbeOne(record, held);
                    });

  if (rules.mapped) {
    AnnounceRuns(plan.runs);
  }

  for (size_t index = 0; index < plan.alone; index++) {
    ProbeOne(chunk[index], {});
  }
  for (const MftRun& run : plan.runs) {
    ProbeRun(run, plan.to_read, keeps);
  }

  // What nothing read, such as a block the cache already held, is withdrawn.
  if (!plan.runs.empty()) {
    volume_->CancelPrefetch();
  }
}

template <Cache::Strategy S>
void EntryProber<S>::AnnounceRuns(std::span<const MftRun> runs) {
  // A forged slot makes the announcement log: the read of the record logs it,
  // at the entry.
  read_log_.Reset(levels_);
  const Log::Detail::CaptureScope scope(read_log_);
  for (const MftRun& run : runs) {
    volume_->PrefetchMftSlots(run.first_slot,
                              run.last_slot - run.first_slot + 1);
  }
}

template <Cache::Strategy S>
void EntryProber<S>::ProbeRun(const MftRun& run,
                              std::span<const ULONGLONG> to_read, bool keeps) {
  const DWORD record_size = volume_->GetFileRecordSize();
  run_bytes_.resize(gsl::narrow<size_t>(run.last_slot - run.first_slot + 1) *
                    record_size);
  bool read = false;
  {
    read_log_.Reset(levels_);
    const Log::Detail::CaptureScope scope(read_log_);
    const Bench::Detail::KindScope kind(Bench::ReadKind::ProbeRun);
    read = volume_->ReadMftSlots(run.first_slot, run_bytes_);
  }
  if (read && keeps) {
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
  if (store_ != nullptr && store_->ran) {
    store_->ran(run.first_slot, run.last_slot - run.first_slot + 1);
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
