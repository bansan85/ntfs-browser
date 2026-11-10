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

// Levels of a directory's B+ tree that the read ahead goes down: the depth
// limit of the walk, which never enters a deeper block.
constexpr std::size_t max_read_levels = 64;

// Share of walk.read_ahead_bytes that the blocks of one directory may hold
// below the first level: a big directory MUST NOT starve the plan of the next
// ones.
constexpr std::uint64_t directory_share_divisor = 4;

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
      cache_block_bytes_(CacheBlockBytes(volume.GetOptions().tuning.cache)) {}

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
void BatchReader<S>::ReadUpcoming(std::span<const ULONGLONG> upcoming) {
  const DWORD record_size = volume_->GetFileRecordSize();

  // The directories that wait at the top of the listing's stack.
  const std::size_t window = (std::min)(upcoming.size(), walk_.read_ahead_dirs);
  std::vector<ULONGLONG> wanted;
  std::unordered_set<ULONGLONG> seen;
  std::size_t held = 0;
  for (const ULONGLONG record : upcoming.first(window)) {
    if (store_->Contains(record)) {
      held++;
    } else if (record >= static_cast<ULONGLONG>(Mft::Idx::User) &&
               !unreadable_.contains(record) && seen.insert(record).second) {
      wanted.push_back(record);
    }
  }
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

  // What the budget has room for. A record costs its size.
  const std::uint64_t held_bytes = store_->Bytes();
  const std::uint64_t room = walk_.read_ahead_bytes > held_bytes
                                 ? walk_.read_ahead_bytes - held_bytes
                                 : 0;
  const std::uint64_t fits = room / record_size;
  if (fits == 0) {
    return;
  }
  if (wanted.size() > fits) {
    wanted.resize(gsl::narrow<std::size_t>(fits));
  }
  Quietly([this, &wanted] { ReadRecordsAhead(wanted); });
}

template <Cache::Strategy S>
void BatchReader<S>::ReadRootBlocks(ULONGLONG directory,
                                    Attr::AttrIndexAlloc<S>* alloc,
                                    std::vector<ULONGLONG> vcns) {
  Quietly([this, directory, alloc, &vcns] {
    const std::vector<Candidate> candidates{
        {.directory = directory, .alloc = alloc, .vcns = std::move(vcns)}};
    ReadBlocks(candidates);
  });
}

template <Cache::Strategy S>
void BatchReader<S>::ReadRecordsAhead(std::span<const ULONGLONG> wanted) {
  const std::vector<ULONGLONG> fetched = FetchRecords(wanted);

  // A raw attribute callback is the caller's: it MUST NOT see a record twice.
  if (!volume_->HasAttrRawCB()) {
    std::vector<std::unique_ptr<FileRecord<S>>> owners;
    const std::vector<Candidate> candidates = FindCandidates(fetched, owners);
    ReadBlocks(candidates);
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

template <Cache::Strategy S>
void BatchReader<S>::AnnounceExtent(const Alloc::Extent& extent) {
  const ULONGLONG cluster_size = volume_->GetClusterSize();
  ULONGLONG start = extent.lcn * cluster_size;
  ULONGLONG end = start + (extent.clusters * cluster_size);
  if constexpr (S == Cache::Strategy::FullCache) {
    // The cache reads whole blocks: two index blocks of one cache block are
    // one announcement, or the second would be read for nothing.
    start -= start % cache_block_bytes_;
    end +=
        (cache_block_bytes_ - (end % cache_block_bytes_)) % cache_block_bytes_;
    if (announced_end_ && start < *announced_end_) {
      start = *announced_end_;
    }
    if (start >= end) {
      return;
    }
    announced_end_ = end;
  }
  volume_->Prefetch(start, end - start);
}

template <Cache::Strategy S>
void BatchReader<S>::ReadBlocks(std::span<const Candidate> candidates) {
  std::vector<Candidate> level(candidates.begin(), candidates.end());
  for (std::size_t depth = 0; !level.empty() && depth < max_read_levels;
       depth++) {
    const std::vector<Want> read = ReadLevel(level, depth != 0);
    level = ChildrenOf(read);
  }
}

template <Cache::Strategy S>
std::vector<typename BatchReader<S>::Want>
    BatchReader<S>::WantsOf(const Candidate& candidate,
                            DWORD block_size) const {
  std::vector<Want> own;
  for (const ULONGLONG vcn : candidate.vcns) {
    if (store_->ContainsBlock(candidate.directory, vcn)) {
      continue;
    }
    const std::optional<ULONGLONG> offset = candidate.alloc->BlockOffset(vcn);
    if (!offset) {
      continue;
    }
    std::vector<Alloc::Extent> extents =
        candidate.alloc->ExtentsOfRange(*offset, block_size);
    // A sparse block reads as zeros, with no read of the disk.
    if (extents.empty()) {
      continue;
    }
    own.push_back({.directory = candidate.directory,
                   .alloc = candidate.alloc,
                   .vcn = vcn,
                   .offset = *offset,
                   .extents = std::move(extents)});
  }
  return own;
}

template <Cache::Strategy S>
void BatchReader<S>::FitToShare(std::vector<Want>& own, ULONGLONG directory,
                                std::uint64_t planned, DWORD block_size) const {
  const std::uint64_t share = walk_.read_ahead_bytes / directory_share_divisor;
  const std::uint64_t kept = store_->BytesOf(directory);
  const std::uint64_t room_directory = share > kept ? share - kept : 0;
  const std::uint64_t used = store_->Bytes() + planned;
  const std::uint64_t room_total =
      walk_.read_ahead_bytes > used ? walk_.read_ahead_bytes - used : 0;
  const std::uint64_t fits =
      (std::min)(room_directory, room_total) / block_size;
  if (own.size() > fits) {
    own.resize(gsl::narrow<std::size_t>(fits));
  }
}

template <Cache::Strategy S>
std::vector<typename BatchReader<S>::Want>
    BatchReader<S>::ReadLevel(std::span<const Candidate> level,
                              bool below_first) {
  const DWORD block_size = volume_->GetIndexBlockSize();
  if (block_size == 0 || volume_->GetClusterSize() == 0) {
    return {};
  }

  std::vector<Want> wants;
  std::uint64_t planned = 0;
  for (const Candidate& candidate : level) {
    std::vector<Want> own = WantsOf(candidate, block_size);
    if (below_first) {
      FitToShare(own, candidate.directory, planned, block_size);
    }
    const std::uint64_t cost = own.size() * std::uint64_t{block_size};
    // A directory whose blocks alone outgrow the budget is left to the walk.
    if (cost > walk_.read_ahead_bytes) {
      continue;
    }
    // The blocks of a directory are read all or none, and planning stops at
    // the first that does not fit. Below the first level, own was cut to fit.
    if (store_->Bytes() + planned + cost > walk_.read_ahead_bytes) {
      break;
    }
    planned += cost;
    std::ranges::move(own, std::back_inserter(wants));
  }

  // The order of the disk: one sweep.
  std::ranges::stable_sort(wants, [](const Want& left, const Want& right) {
    return left.extents.front().lcn < right.extents.front().lcn;
  });
  return HoldWants(wants, block_size);
}

template <Cache::Strategy S>
std::vector<typename BatchReader<S>::Want>
    BatchReader<S>::HoldWants(std::span<Want> wants, DWORD block_size) {
  std::vector<Want> held_wants;
  const std::size_t round =
      (std::max<std::size_t>)(1, probe_.max_chunk_records);
  for (std::size_t first = 0; first < wants.size(); first += round) {
    const std::span<Want> chunk =
        wants.subspan(first, (std::min)(round, wants.size() - first));

    capture_->Reset();
    announced_end_.reset();
    for (const Want& want : chunk) {
      for (const Alloc::Extent& extent : want.extents) {
        AnnounceExtent(extent);
      }
    }
    std::vector<BYTE> bytes(block_size);
    for (Want& want : chunk) {
      if (want.alloc->ReadBlockBytes(want.offset, bytes)) {
        store_->HoldBlock(want.directory, want.vcn, bytes);
        held_wants.push_back(std::move(want));
      }
    }
    volume_->CancelPrefetch();
  }
  return held_wants;
}

template <Cache::Strategy S>
void BatchReader<S>::AddSubNodes(const Want& want,
                                 std::unordered_set<ULONGLONG>& seen,
                                 Candidate& child) {
  want.alloc->SetBlockSource(
      [this](ULONGLONG owner, ULONGLONG vcn) {
        return store_->PeekBlock(owner, vcn);
      },
      want.directory);
  Attr::IndexBlock block;
  if (want.alloc->ParseIndexBlock(want.vcn, block)) {
    for (const IndexEntryView& entry : block) {
      if (entry.IsSubNodePtr() && seen.insert(entry.GetSubNodeVCN()).second) {
        child.vcns.push_back(entry.GetSubNodeVCN());
      }
    }
  }
  want.alloc->SetBlockSource({}, 0);
}

template <Cache::Strategy S>
std::vector<typename BatchReader<S>::Candidate>
    BatchReader<S>::ChildrenOf(std::span<const Want> read) {
  std::vector<Candidate> next;
  std::unordered_map<ULONGLONG, std::size_t> place;
  // A VCN is read once, whatever number of blocks name it.
  std::unordered_map<ULONGLONG, std::unordered_set<ULONGLONG>> seen;
  for (const Want& want : read) {
    const auto [at, added] = place.try_emplace(want.directory, next.size());
    if (added) {
      next.push_back(
          {.directory = want.directory, .alloc = want.alloc, .vcns = {}});
    }
    AddSubNodes(want, seen[want.directory], next.at(at->second));
  }
  std::erase_if(next,
                [](const Candidate& child) { return child.vcns.empty(); });
  return next;
}

template class BatchReader<Cache::Strategy::NoCache>;
template class BatchReader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
