#include "io/batch-reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/base.h"
#include "attr/index-alloc.h"
#include "attr/index-block.h"
#include "attr/mask.h"
#include "bench/bench.h"
#include "bench/recorder.h"
#include "cache/strategy.h"
#include "index-entry.h"
#include "io/directory-read-ahead.h"
#include "io/file-record-index.h"
#include "io/file-record.h"
#include "io/mft-runs.h"
#include "io/probe-lane.h"
#include "io/read-ahead-held.h"
#include "log/ntfs-common.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Most refs the set of records that could not be read ahead holds. Past it the
// set is emptied: a record is then tried again, which costs one read.
constexpr std::size_t max_unreadable_records = std::size_t{1} << 16U;

}  // namespace

template <Cache::Strategy S>
BatchReader<S>::BatchReader(const NtfsVolume<S>& volume,
                            ReadAheadShared& shared, ProbeLane<S>& lane)
    : volume_(&volume),
      store_(&shared.store),
      keeper_(&shared.keeper),
      lane_(&lane),
      capture_(&shared.capture),
      stats_(&shared.stats),
      walk_(volume.GetOptions().tuning.walk),
      probe_(volume.GetOptions().tuning.probe),
      blocks_(volume, shared) {}

template <Cache::Strategy S>
void BatchReader<S>::Quietly(const std::function<void()>& body) {
  volume_->SetReadPattern(Tuning::Pattern::DependentSmall);
  const std::uint64_t recovered_before = Log::Detail::RecoveredCount();
  stats_->batches++;
  try {
    capture_->Reset();
    const Log::Detail::CaptureScope scope(capture_->log);
    body();
  } catch (const std::exception&) {
    // A failed read ahead costs the gain only: the listing reads what is not
    // held.
  }
  Log::Detail::ForgetRecoveredSince(
      {.lines = 0, .recovered = recovered_before});
}

template <Cache::Strategy S>
std::vector<ULONGLONG>
    BatchReader<S>::WantedOf(std::span<const ULONGLONG> upcoming,
                             std::size_t& held) const {
  // The directories that wait at the top of the listing's stack.
  const std::size_t window = (std::min)(upcoming.size(), walk_.read_ahead_dirs);
  std::vector<ULONGLONG> wanted;
  std::unordered_set<ULONGLONG> seen;
  for (const ULONGLONG record : upcoming.first(window)) {
    if (store_->Contains(record)) {
      held++;
    } else if (record >= static_cast<ULONGLONG>(Mft::Idx::User) &&
               !unreadable_.contains(record) && seen.insert(record).second) {
      wanted.push_back(record);
    }
  }
  return wanted;
}

template <Cache::Strategy S>
bool BatchReader<S>::FitToBudget(std::vector<ULONGLONG>& wanted) const {
  const std::uint64_t held_bytes = store_->Bytes();
  const std::uint64_t room = walk_.read_ahead_bytes > held_bytes
                                 ? walk_.read_ahead_bytes - held_bytes
                                 : 0;
  const std::uint64_t fits = room / volume_->GetFileRecordSize();
  if (wanted.size() > fits) {
    wanted.resize(gsl::narrow<std::size_t>(fits));
  }
  return fits != 0;
}

template <Cache::Strategy S>
void BatchReader<S>::ReadUpcoming(std::span<const ULONGLONG> upcoming) {
  std::size_t held = 0;
  std::vector<ULONGLONG> wanted = WantedOf(upcoming, held);
  if (wanted.empty()) {
    return;
  }
  // A batch reads several directories at once: it waits until the ones held
  // ahead are down to the half of the window. But the listing enters the
  // first one next, and would read it alone: that one is read at once.
  const std::size_t low_water = (walk_.read_ahead_dirs + 1) / 2;
  if (held >= low_water) {
    // One only: more would take directories from the big sorted batches.
    if (wanted.front() != upcoming.front()) {
      return;
    }
    wanted.resize(1);
  }

  if (FitToBudget(wanted)) {
    Quietly([this, &wanted] { ReadRecordsAhead(wanted); });
  }
}

template <Cache::Strategy S>
void BatchReader<S>::ReadRootBlocks(ULONGLONG directory,
                                    Attr::AttrIndexAlloc<S>* alloc,
                                    std::vector<ULONGLONG> vcns) {
  Quietly([this, directory, alloc, &vcns] {
    const std::vector<Candidate> candidates{
        {.directory = directory, .alloc = alloc, .vcns = std::move(vcns)}};
    blocks_.ReadBlocks(candidates);
  });
}

template <Cache::Strategy S>
void BatchReader<S>::ReadRecordsAhead(std::span<const ULONGLONG> wanted) {
  const std::vector<ULONGLONG> fetched = FetchRecords(wanted);

  // A raw attribute callback is the caller's: it MUST NOT see a record twice.
  if (!volume_->HasAttrRawCB()) {
    std::vector<std::unique_ptr<FileRecord<S>>> owners;
    const std::vector<Candidate> candidates = FindCandidates(fetched, owners);
    blocks_.ReadBlocks(candidates);
    // Each owner holds the attributes of the candidate of its place.
    for (std::size_t index = 0; index < candidates.size(); index++) {
      lane_->PlanRunsOf(candidates.at(index).directory, *owners.at(index));
    }
  }
  // Its attributes log when they are destroyed: that MUST happen here.
  spare_.reset();
}

template <Cache::Strategy S>
std::unique_ptr<FileRecord<S>> BatchReader<S>::MakeScratch() {
  auto scratch = std::make_unique<FileRecord<S>>(*volume_);
  // The attributes a listing reads, and no more.
  scratch->SetParseOptions(
      {.attr_mask = Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation,
       .read_refusal = &refused_});
  return scratch;
}

template <Cache::Strategy S>
std::vector<ULONGLONG>
    BatchReader<S>::FetchRecords(std::span<const ULONGLONG> wanted) {
  std::vector<ULONGLONG> sorted(wanted.begin(), wanted.end());
  std::ranges::sort(sorted);

  const std::size_t round =
      (std::max<std::size_t>)(1, probe_.max_chunk_records);
  for (std::size_t first = 0; first < sorted.size(); first += round) {
    HoldFromWindow(std::span<const ULONGLONG>(sorted).subspan(
        first, (std::min)(round, sorted.size() - first)));
    ReadMissing();
  }

  std::vector<ULONGLONG> fetched;
  std::ranges::copy_if(
      wanted, std::back_inserter(fetched),
      [this](ULONGLONG record) { return store_->HasRecord(record); });
  return fetched;
}

template <Cache::Strategy S>
void BatchReader<S>::HoldFromWindow(std::span<const ULONGLONG> chunk) {
  // A record that a probe read lately is held from the window.
  missing_.clear();
  for (const ULONGLONG record : chunk) {
    const std::span<const BYTE> bytes = keeper_->FindInWindow(record);
    if (bytes.empty()) {
      missing_.push_back(record);
    } else {
      store_->HoldRecord(record, bytes);
    }
  }
}

template <Cache::Strategy S>
std::vector<MftRun> BatchReader<S>::PlanMissingRuns() const {
  const DWORD record_size = volume_->GetFileRecordSize();
  std::vector<MftRun> runs =
      PlanMftRuns(missing_, probe_.max_gap_bytes / record_size,
                  probe_.max_run_bytes / record_size,
                  WideGapOf(probe_, volume_->GetMediaKind(), record_size,
                            S == Cache::Strategy::FullCache));
  // A run that the window keeps is kept whole: the disk reads its clusters
  // anyway.
  if (keeper_->WindowEnabled()) {
    runs = WidenToClusters(std::move(runs),
                           volume_->GetClusterSize() / record_size,
                           static_cast<ULONGLONG>(Mft::Idx::User));
  }
  return runs;
}

template <Cache::Strategy S>
void BatchReader<S>::ReadMissing() {
  const std::vector<MftRun> runs = PlanMissingRuns();
  capture_->Reset();
  for (const MftRun& run : runs) {
    volume_->PrefetchMftSlots(run.first_slot,
                              run.last_slot - run.first_slot + 1);
  }
  for (const MftRun& run : runs) {
    ReadRun(run);
  }
  // What nothing read, such as a block the cache already held, is withdrawn.
  volume_->CancelPrefetch();
}

template <Cache::Strategy S>
void BatchReader<S>::ReadRun(const MftRun& run) {
  const DWORD record_size = volume_->GetFileRecordSize();
  run_bytes_.resize(
      gsl::narrow<std::size_t>(run.last_slot - run.first_slot + 1) *
      record_size);
  bool read = false;
  {
    const Bench::Detail::KindScope kind(Bench::ReadKind::Record);
    read = volume_->ReadMftSlots(run.first_slot, run_bytes_);
  }
  if (read) {
    keeper_->KeepRun(run.first_slot, run_bytes_);
  }
  for (std::size_t index = run.begin; index < run.end; index++) {
    const ULONGLONG record = missing_[index];
    if (!read) {
      NoteUnreadable(record);
      continue;
    }
    const auto offset =
        gsl::narrow<std::size_t>(record - run.first_slot) * record_size;
    store_->HoldRecord(
        record, std::span<const BYTE>(run_bytes_).subspan(offset, record_size));
  }
}

template <Cache::Strategy S>
void BatchReader<S>::NoteUnreadable(ULONGLONG record) {
  if (unreadable_.size() >= max_unreadable_records) {
    unreadable_.clear();
  }
  unreadable_.insert(record);
}

template <Cache::Strategy S>
std::vector<typename BatchReader<S>::Candidate> BatchReader<S>::FindCandidates(
    std::span<const ULONGLONG> fetched,
    std::vector<std::unique_ptr<FileRecord<S>>>& owners) {
  std::vector<Candidate> candidates;
  for (const ULONGLONG record : fetched) {
    if (!spare_) {
      spare_ = MakeScratch();
    }
    capture_->Reset();
    refused_ = false;
    FileRecord<S>& scratch = *spare_;
    // A record that fails its update sequence check is untrusted: the listing
    // reads its blocks itself, as it judges the record.
    if (!scratch.ParseFileRecord(record, store_->RecordOf(record)) ||
        scratch.HasFailedFixup() || !scratch.ParseAttrs() || refused_) {
      continue;
    }
    std::optional<typename DirectoryIndex<S>::RootChildren> children =
        DirectoryIndex<S>(scratch).RootChildBlocks();
    if (!children) {
      // No block to read, and the probes of its entries are known.
      lane_->PlanRunsOf(record, scratch);
      continue;
    }
    candidates.push_back({.directory = record,
                          .alloc = children->alloc,
                          .vcns = std::move(children->vcns)});
    // Its attributes are the ones the blocks are read through.
    owners.push_back(std::move(spare_));
  }
  return candidates;
}

template class BatchReader<Cache::Strategy::NoCache>;
template class BatchReader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
