#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>

#include "attr/index-alloc.h"
#include "attr/index-block.h"
#include "bench/recorder.h"
#include "io/file-record-index.h"
#include "io/mft-runs.h"
#include "io/record-memo.h"
#include "io/record-window.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Smallest block a FullCache reads, as FileReader keeps it: one sector of the
// largest size NTFS supports.
constexpr std::size_t min_cache_block_bytes = 4096;

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
class DirectoryReadAhead<S>::Impl {
 public:
  explicit Impl(const NtfsVolume<S>& volume)
      : volume_(&volume),
        walk_(volume.GetOptions().tuning.walk),
        probe_(volume.GetOptions().tuning.probe),
        cache_block_bytes_(
            (std::max)(volume.GetOptions().tuning.cache.block_bytes,
                       min_cache_block_bytes)),
        levels_(Log::Detail::EnabledLevels()),
        // A FullCache volume holds what it read in its block cache already.
        window_(S == Cache::Strategy::NoCache ? walk_.record_window_bytes : 0,
                volume.GetFileRecordSize()),
        memo_(S == Cache::Strategy::NoCache ? walk_.record_memo_entries : 0,
              volume.GetFileRecordSize()) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() { Detach(); }

  [[nodiscard]] bool Enabled() const noexcept {
    return walk_.read_ahead_dirs != 0;
  }

  [[nodiscard]] std::size_t Window() const noexcept {
    return walk_.read_ahead_dirs;
  }

  void Attach(FileRecord<S>& listing);
  void Begin();
  void Plan(std::span<const ULONGLONG> upcoming);
  void Release(ULONGLONG directory);

  [[nodiscard]] DirectoryReadAheadStats GetStats() const noexcept {
    DirectoryReadAheadStats stats = stats_;
    stats.held_bytes = held_bytes_;
    return stats;
  }

 private:
  // One index block held, and whether the listing took it.
  struct HeldBlock {
    std::vector<BYTE> bytes;
    bool served{false};
  };

  // What is held for one directory.
  struct HeldDirectory {
    // The raw bytes of its file record, empty when only blocks are held.
    std::vector<BYTE> record;
    bool record_served{false};
    // Its index blocks, by sub-node VCN.
    std::unordered_map<ULONGLONG, HeldBlock> blocks;
  };

  // A directory whose blocks may be read ahead: its $INDEX_ALLOCATION, and the
  // sub-node VCNs of its $INDEX_ROOT. alloc belongs to a record that outlives
  // the batch.
  struct Candidate {
    ULONGLONG directory;
    Attr::AttrIndexAlloc<S>* alloc;
    std::vector<ULONGLONG> vcns;
  };

  // One block to read ahead.
  struct Want {
    ULONGLONG directory;
    Attr::AttrIndexAlloc<S>* alloc;
    ULONGLONG vcn;
    // Byte offset of the block in its $INDEX_ALLOCATION.
    ULONGLONG offset;
    // The runs of clusters that hold it, in the order a read goes through them.
    std::vector<Alloc::Extent> extents;
  };

  // The bytes the listing's record asks for, empty when not held.
  [[nodiscard]] std::span<const BYTE> HeldRecordBytes(ULONGLONG record);
  // The bytes of record in the window, empty when it does not hold them.
  [[nodiscard]] std::span<const BYTE> FindInWindow(ULONGLONG record);
  // A record that a probe may judge: the window's bytes, or the few bytes of a
  // probe that the memo kept of it. Empty when neither holds it.
  [[nodiscard]] std::span<const BYTE> FindForProbe(ULONGLONG record);
  // Gives a run of $MFT that was read to the window and to the memo.
  void KeepRun(ULONGLONG first_record, std::span<const BYTE> bytes);
  [[nodiscard]] std::span<const BYTE> HeldBlockBytes(ULONGLONG directory,
                                                     ULONGLONG vcn);

  // Takes the sources off the attached record, and off its current index
  // allocation.
  void Detach() noexcept;

  // A record of its own, in which a held record is parsed to find its blocks.
  [[nodiscard]] std::unique_ptr<FileRecord<S>> MakeScratch();

  // Holds a copy of the bytes of the file record record.
  void Hold(ULONGLONG record, std::span<const BYTE> bytes);

  // Reads and holds the records of wanted, then the blocks they point to.
  void ReadAhead(std::span<const ULONGLONG> wanted);

  // Reads the records of wanted in sorted runs of $MFT, announced first, and
  // holds each. Returns those held, in the order of wanted.
  [[nodiscard]] std::vector<ULONGLONG>
      FetchRecords(std::span<const ULONGLONG> wanted);

  // Finds the directories among the held records whose blocks may be read
  // ahead, parsing each into a scratch record that stays in owners.
  [[nodiscard]] std::vector<Candidate>
      FindCandidates(std::span<const ULONGLONG> fetched,
                     std::vector<std::unique_ptr<FileRecord<S>>>& owners);

  // Reads the blocks of candidates sorted by place on the disk, announced
  // first, and holds each, then the blocks those name, level by level down to
  // the leaves. Stops at the byte budget.
  void ReadBlocks(std::span<const Candidate> candidates);

  // Reads and holds the blocks that level names, one level of the trees. A
  // level below the first stops at a quarter of the byte budget for each
  // directory. Returns the blocks it held.
  [[nodiscard]] std::vector<Want> ReadLevel(std::span<const Candidate> level,
                                            bool below_first);

  // The sub-node VCNs of the held blocks in read: the next level of the trees.
  // Parses each block as the walk does, and drops what that logs.
  [[nodiscard]] std::vector<Candidate> ChildrenOf(std::span<const Want> read);

  // The bytes of a held block, without counting the listing as served.
  [[nodiscard]] std::span<const BYTE> PeekBlockBytes(ULONGLONG directory,
                                                     ULONGLONG vcn) const;

  // Announces the disk range of extent, unless an earlier announcement of the
  // round covers it.
  void AnnounceExtent(const Alloc::Extent& extent);

  // Bytes of the record or block of a directory that is dropped, to free.
  [[nodiscard]] static std::uint64_t BytesOf(const HeldDirectory& held);

  const NtfsVolume<S>* volume_;
  Tuning::WalkTuning walk_;
  Tuning::ProbeTuning probe_;
  // Bytes of one block the FullCache reads.
  std::size_t cache_block_bytes_;
  // The levels the sinks accept, taken once: a capture MUST NOT ask them.
  Log::Detail::LevelMask levels_;
  // The runs of $MFT that the probes and the batches read last.
  RecordWindow window_;
  // What a probe reads of the records of the runs that went through the window,
  // for many more of them.
  RecordMemo memo_;
  FileRecord<S>* listing_{nullptr};
  std::unordered_map<ULONGLONG, HeldDirectory> held_;
  std::uint64_t held_bytes_{0};
  DirectoryReadAheadStats stats_;
  // Records that a read ahead failed to read: the listing reads them itself,
  // and they are not tried again.
  std::unordered_set<ULONGLONG> unreadable_;

  // What the batch under way logs, to be dropped.
  Log::Detail::CapturedLog log_;
  // Set by a scratch record that a parse would have read the volume for.
  bool refused_{false};
  // The scratch record the next held record is parsed in.
  std::unique_ptr<FileRecord<S>> spare_;
  // The bytes of the run being read.
  std::vector<BYTE> run_bytes_;
  // The records of a round that the window did not hold.
  std::vector<ULONGLONG> missing_;
  // End of the last announcement of the round, where blocks cannot overlap.
  std::optional<ULONGLONG> announced_end_;
};

template <Cache::Strategy S>
std::span<const BYTE>
    DirectoryReadAhead<S>::Impl::HeldRecordBytes(ULONGLONG record) {
  const auto held = held_.find(record);
  if (held == held_.end() || held->second.record.empty()) {
    return FindInWindow(record);
  }
  if (!held->second.record_served) {
    held->second.record_served = true;
    stats_.records_served++;
  }
  return held->second.record;
}

template <Cache::Strategy S>
std::span<const BYTE>
    DirectoryReadAhead<S>::Impl::FindInWindow(ULONGLONG record) {
  const std::span<const BYTE> bytes = window_.Find(record);
  if (!bytes.empty()) {
    stats_.window_records++;
  }
  return bytes;
}

template <Cache::Strategy S>
std::span<const BYTE>
    DirectoryReadAhead<S>::Impl::FindForProbe(ULONGLONG record) {
  const std::span<const BYTE> bytes = FindInWindow(record);
  if (!bytes.empty()) {
    return bytes;
  }
  const std::span<const BYTE> kept = memo_.Find(record);
  if (!kept.empty()) {
    stats_.memo_records++;
  }
  return kept;
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::KeepRun(ULONGLONG first_record,
                                          std::span<const BYTE> bytes) {
  window_.Put(first_record, bytes);
  memo_.Put(first_record, bytes);
}

template <Cache::Strategy S>
std::span<const BYTE>
    DirectoryReadAhead<S>::Impl::HeldBlockBytes(ULONGLONG directory,
                                                ULONGLONG vcn) {
  const auto held = held_.find(directory);
  if (held == held_.end()) {
    return {};
  }
  const auto block = held->second.blocks.find(vcn);
  if (block == held->second.blocks.end()) {
    return {};
  }
  if (!block->second.served) {
    block->second.served = true;
    stats_.blocks_served++;
  }
  return block->second.bytes;
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Attach(FileRecord<S>& listing) {
  if (!Enabled()) {
    return;
  }
  listing_ = &listing;
  listing.SetRecordSource(
      [this](ULONGLONG record) { return HeldRecordBytes(record); });
  listing.SetIndexBlockSource([this](ULONGLONG directory, ULONGLONG vcn) {
    return HeldBlockBytes(directory, vcn);
  });
  if (window_.Enabled() || memo_.Enabled()) {
    listing.SetProbeStore(
        {.find = [this](ULONGLONG record) { return FindForProbe(record); },
         .keep =
             [this](ULONGLONG first_record, std::span<const BYTE> bytes) {
               KeepRun(first_record, bytes);
             }});
  }
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Detach() noexcept {
  if (listing_ == nullptr) {
    return;
  }
  listing_->SetRecordSource({});
  listing_->SetIndexBlockSource({});
  listing_->SetProbeStore({});
  for (const auto& attr : listing_->GetAttr(Attr::Type::IndexAllocation)) {
    static_cast<Attr::AttrIndexAlloc<S>*>(attr.get())->SetBlockSource({}, 0);
  }
  listing_ = nullptr;
}

template <Cache::Strategy S>
std::unique_ptr<FileRecord<S>> DirectoryReadAhead<S>::Impl::MakeScratch() {
  auto scratch = std::make_unique<FileRecord<S>>(*volume_);
  // The attributes a listing reads, and no more.
  scratch->SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  scratch->SetReadRefusal(&refused_);
  return scratch;
}

template <Cache::Strategy S>
std::uint64_t DirectoryReadAhead<S>::Impl::BytesOf(const HeldDirectory& held) {
  std::uint64_t bytes = held.record.size();
  for (const auto& [vcn, block] : held.blocks) {
    bytes += block.bytes.size();
  }
  return bytes;
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Release(ULONGLONG directory) {
  const auto held = held_.find(directory);
  if (held == held_.end()) {
    return;
  }
  if (!held->second.record.empty() && !held->second.record_served) {
    stats_.records_unused++;
  }
  for (const auto& [vcn, block] : held->second.blocks) {
    if (!block.served) {
      stats_.blocks_unused++;
    }
  }
  held_bytes_ -= BytesOf(held->second);
  held_.erase(held);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Plan(std::span<const ULONGLONG> upcoming) {
  const DWORD record_size = volume_->GetFileRecordSize();
  if (!Enabled() || listing_ == nullptr || record_size == 0 ||
      !volume_->HasMftBase()) {
    return;
  }

  // The directories that wait at the top of the listing's stack.
  const std::size_t window = (std::min)(upcoming.size(), walk_.read_ahead_dirs);
  std::vector<ULONGLONG> wanted;
  std::unordered_set<ULONGLONG> seen;
  std::size_t held = 0;
  for (const ULONGLONG record : upcoming.first(window)) {
    if (held_.contains(record)) {
      held++;
    } else if (record >= static_cast<ULONGLONG>(Mft::Idx::User) &&
               !unreadable_.contains(record) && seen.insert(record).second) {
      wanted.push_back(record);
    }
  }
  // A batch reads several directories at once: it waits until the ones held
  // ahead are down to the half of the window.
  const std::size_t low_water = (walk_.read_ahead_dirs + 1) / 2;
  if (wanted.empty() || held >= low_water) {
    return;
  }

  // What the budget has room for. A record costs its size.
  const std::uint64_t room = walk_.read_ahead_bytes > held_bytes_
                                 ? walk_.read_ahead_bytes - held_bytes_
                                 : 0;
  const std::uint64_t fits = room / record_size;
  if (fits == 0) {
    return;
  }
  if (wanted.size() > fits) {
    wanted.resize(gsl::narrow<std::size_t>(fits));
  }
  ReadAhead(wanted);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Begin() {
  if (!Enabled() || listing_ == nullptr || volume_->HasAttrRawCB()) {
    return;
  }
  const std::optional<ULONGLONG> directory = listing_->GetFileReference();
  if (!directory || held_.contains(*directory)) {
    return;
  }
  std::optional<typename DirectoryIndex<S>::RootChildren> children =
      DirectoryIndex<S>(*listing_).RootChildBlocks();
  if (!children) {
    return;
  }

  volume_->SetReadPattern(Tuning::Pattern::DependentSmall);
  const std::uint64_t recovered_before = Log::Detail::RecoveredCount();
  stats_.batches++;
  try {
    log_.Reset(levels_);
    const Log::Detail::CaptureScope scope(log_);
    const std::vector<Candidate> candidates{
        {.directory = *directory,
         .alloc = children->alloc,
         .vcns = std::move(children->vcns)}};
    ReadBlocks(candidates);
  } catch (const std::exception&) {
    // A failed read ahead costs the gain only: the listing reads what is not
    // held.
  }
  Log::Detail::ForgetRecoveredSince(
      {.lines = 0, .recovered = recovered_before});

  // The attributes already exist: they take their source from here on.
  for (const auto& attr : listing_->GetAttr(Attr::Type::IndexAllocation)) {
    static_cast<Attr::AttrIndexAlloc<S>*>(attr.get())
        ->SetBlockSource(
            [this](ULONGLONG owner, ULONGLONG vcn) {
              return HeldBlockBytes(owner, vcn);
            },
            *directory);
  }
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::ReadAhead(std::span<const ULONGLONG> wanted) {
  volume_->SetReadPattern(Tuning::Pattern::DependentSmall);
  // The batch MUST NOT log, and what it counts as salvaged is counted again
  // when the listing reaches the record.
  const std::uint64_t recovered_before = Log::Detail::RecoveredCount();
  stats_.batches++;
  try {
    log_.Reset(levels_);
    const Log::Detail::CaptureScope scope(log_);
    const std::vector<ULONGLONG> fetched = FetchRecords(wanted);

    // A raw attribute callback is the caller's: it MUST NOT see a record twice.
    if (!volume_->HasAttrRawCB()) {
      std::vector<std::unique_ptr<FileRecord<S>>> owners;
      const std::vector<Candidate> candidates = FindCandidates(fetched, owners);
      ReadBlocks(candidates);
    }
    // Its attributes log when they are destroyed: that MUST happen here.
    spare_.reset();
  } catch (const std::exception&) {
    // A failed read ahead costs the gain only: the listing reads what is not
    // held.
  }
  Log::Detail::ForgetRecoveredSince(
      {.lines = 0, .recovered = recovered_before});
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Hold(ULONGLONG record,
                                       std::span<const BYTE> bytes) {
  HeldDirectory& held = held_[record];
  held.record.assign(bytes.begin(), bytes.end());
  held_bytes_ += held.record.size();
  stats_.peak_held_bytes = (std::max)(stats_.peak_held_bytes, held_bytes_);
  stats_.records_read++;
}

template <Cache::Strategy S>
std::vector<ULONGLONG> DirectoryReadAhead<S>::Impl::FetchRecords(
    std::span<const ULONGLONG> wanted) {
  const DWORD record_size = volume_->GetFileRecordSize();
  std::vector<ULONGLONG> sorted(wanted.begin(), wanted.end());
  std::ranges::sort(sorted);

  const std::size_t round =
      (std::max<std::size_t>)(1, probe_.max_chunk_records);
  for (std::size_t first = 0; first < sorted.size(); first += round) {
    const std::span<const ULONGLONG> chunk =
        std::span<const ULONGLONG>(sorted).subspan(
            first, (std::min)(round, sorted.size() - first));
    // A record that a probe read lately is held from the window.
    missing_.clear();
    for (const ULONGLONG record : chunk) {
      const std::span<const BYTE> bytes = FindInWindow(record);
      if (bytes.empty()) {
        missing_.push_back(record);
      } else {
        Hold(record, bytes);
      }
    }
    std::vector<MftRun> runs =
        PlanMftRuns(missing_, probe_.max_gap_bytes / record_size,
                    probe_.max_run_bytes / record_size,
                    WideGapOf(probe_, volume_->GetMediaKind(), record_size,
                              S == Cache::Strategy::FullCache));
    // A run that the window keeps is kept whole: the disk reads its clusters
    // anyway.
    if (window_.Enabled()) {
      runs = WidenToClusters(std::move(runs),
                             volume_->GetClusterSize() / record_size,
                             static_cast<ULONGLONG>(Mft::Idx::User));
    }

    log_.Reset(levels_);
    for (const MftRun& run : runs) {
      volume_->PrefetchMftSlots(run.first_slot,
                                run.last_slot - run.first_slot + 1);
    }
    for (const MftRun& run : runs) {
      run_bytes_.resize(
          gsl::narrow<std::size_t>(run.last_slot - run.first_slot + 1) *
          record_size);
      bool read = false;
      {
        const Bench::Detail::KindScope kind(Bench::ReadKind::Record);
        read = volume_->ReadMftSlots(run.first_slot, run_bytes_);
      }
      if (read) {
        KeepRun(run.first_slot, run_bytes_);
      }
      for (std::size_t index = run.begin; index < run.end; index++) {
        const ULONGLONG record = missing_[index];
        if (!read) {
          if (unreadable_.size() >= max_unreadable_records) {
            unreadable_.clear();
          }
          unreadable_.insert(record);
          continue;
        }
        const auto offset =
            gsl::narrow<std::size_t>(record - run.first_slot) * record_size;
        const std::span<const BYTE> bytes =
            std::span<const BYTE>(run_bytes_).subspan(offset, record_size);
        Hold(record, bytes);
      }
    }
    // What nothing read, such as a block the cache already held, is withdrawn.
    volume_->CancelPrefetch();
  }

  std::vector<ULONGLONG> fetched;
  for (const ULONGLONG record : wanted) {
    const auto held = held_.find(record);
    if (held != held_.end() && !held->second.record.empty()) {
      fetched.push_back(record);
    }
  }
  return fetched;
}

template <Cache::Strategy S>
std::vector<typename DirectoryReadAhead<S>::Impl::Candidate>
    DirectoryReadAhead<S>::Impl::FindCandidates(
        std::span<const ULONGLONG> fetched,
        std::vector<std::unique_ptr<FileRecord<S>>>& owners) {
  std::vector<Candidate> candidates;
  for (const ULONGLONG record : fetched) {
    if (!spare_) {
      spare_ = MakeScratch();
    }
    log_.Reset(levels_);
    refused_ = false;
    FileRecord<S>& scratch = *spare_;
    // A record that fails its update sequence check is untrusted: the listing
    // reads its blocks itself, as it judges the record.
    if (!scratch.ParseFileRecord(record, held_.at(record).record) ||
        scratch.HasFailedFixup() || !scratch.ParseAttrs() || refused_) {
      continue;
    }
    std::optional<typename DirectoryIndex<S>::RootChildren> children =
        DirectoryIndex<S>(scratch).RootChildBlocks();
    if (!children) {
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
void DirectoryReadAhead<S>::Impl::AnnounceExtent(const Alloc::Extent& extent) {
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
std::span<const BYTE>
    DirectoryReadAhead<S>::Impl::PeekBlockBytes(ULONGLONG directory,
                                                ULONGLONG vcn) const {
  const auto held = held_.find(directory);
  if (held == held_.end()) {
    return {};
  }
  const auto block = held->second.blocks.find(vcn);
  return block == held->second.blocks.end()
             ? std::span<const BYTE>{}
             : std::span<const BYTE>(block->second.bytes);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::ReadBlocks(
    std::span<const Candidate> candidates) {
  std::vector<Candidate> level(candidates.begin(), candidates.end());
  for (std::size_t depth = 0; !level.empty() && depth < max_read_levels;
       depth++) {
    const std::vector<Want> read = ReadLevel(level, depth != 0);
    level = ChildrenOf(read);
  }
}

template <Cache::Strategy S>
std::vector<typename DirectoryReadAhead<S>::Impl::Want>
    DirectoryReadAhead<S>::Impl::ReadLevel(std::span<const Candidate> level,
                                           bool below_first) {
  const DWORD block_size = volume_->GetIndexBlockSize();
  if (block_size == 0 || volume_->GetClusterSize() == 0) {
    return {};
  }

  std::vector<Want> wants;
  std::uint64_t planned = 0;
  for (const Candidate& candidate : level) {
    std::vector<Want> own;
    for (const ULONGLONG vcn : candidate.vcns) {
      const auto held = held_.find(candidate.directory);
      if (held != held_.end() && held->second.blocks.contains(vcn)) {
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
    if (below_first) {
      // What this directory and the budget have room for: the walk reads the
      // blocks that do not fit.
      const auto held = held_.find(candidate.directory);
      const std::uint64_t share =
          walk_.read_ahead_bytes / directory_share_divisor;
      const std::uint64_t kept =
          held == held_.end() ? 0 : BytesOf(held->second);
      const std::uint64_t room_directory = share > kept ? share - kept : 0;
      const std::uint64_t used = held_bytes_ + planned;
      const std::uint64_t room_total =
          walk_.read_ahead_bytes > used ? walk_.read_ahead_bytes - used : 0;
      const std::uint64_t fits =
          (std::min)(room_directory, room_total) / block_size;
      if (own.size() > fits) {
        own.resize(gsl::narrow<std::size_t>(fits));
      }
    }
    const std::uint64_t cost = own.size() * std::uint64_t{block_size};
    // A directory whose blocks alone outgrow the budget is left to the walk.
    if (cost > walk_.read_ahead_bytes) {
      continue;
    }
    // The blocks of a directory are read all or none, and planning stops at
    // the first that does not fit. Below the first level, own was cut to fit.
    if (held_bytes_ + planned + cost > walk_.read_ahead_bytes) {
      break;
    }
    planned += cost;
    std::ranges::move(own, std::back_inserter(wants));
  }

  // The order of the disk: one sweep.
  std::ranges::stable_sort(wants, [](const Want& left, const Want& right) {
    return left.extents.front().lcn < right.extents.front().lcn;
  });

  std::vector<Want> held_wants;
  const std::size_t round =
      (std::max<std::size_t>)(1, probe_.max_chunk_records);
  for (std::size_t first = 0; first < wants.size(); first += round) {
    const std::span<Want> chunk = std::span<Want>(wants).subspan(
        first, (std::min)(round, wants.size() - first));

    log_.Reset(levels_);
    announced_end_.reset();
    for (const Want& want : chunk) {
      for (const Alloc::Extent& extent : want.extents) {
        AnnounceExtent(extent);
      }
    }
    std::vector<BYTE> bytes(block_size);
    for (Want& want : chunk) {
      if (!want.alloc->ReadBlockBytes(want.offset, bytes)) {
        continue;
      }
      HeldDirectory& held = held_[want.directory];
      held.blocks[want.vcn].bytes = bytes;
      held_bytes_ += bytes.size();
      stats_.peak_held_bytes = (std::max)(stats_.peak_held_bytes, held_bytes_);
      stats_.blocks_read++;
      held_wants.push_back(std::move(want));
    }
    volume_->CancelPrefetch();
  }
  return held_wants;
}

template <Cache::Strategy S>
std::vector<typename DirectoryReadAhead<S>::Impl::Candidate>
    DirectoryReadAhead<S>::Impl::ChildrenOf(std::span<const Want> read) {
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
    Candidate& child = next.at(at->second);

    want.alloc->SetBlockSource(
        [this](ULONGLONG owner, ULONGLONG vcn) {
          return PeekBlockBytes(owner, vcn);
        },
        want.directory);
    Attr::IndexBlock block;
    if (want.alloc->ParseIndexBlock(want.vcn, block)) {
      for (const IndexEntryView& entry : block) {
        if (entry.IsSubNodePtr() &&
            seen[want.directory].insert(entry.GetSubNodeVCN()).second) {
          child.vcns.push_back(entry.GetSubNodeVCN());
        }
      }
    }
    want.alloc->SetBlockSource({}, 0);
  }
  std::erase_if(next,
                [](const Candidate& child) { return child.vcns.empty(); });
  return next;
}

template <Cache::Strategy S>
DirectoryReadAhead<S>::DirectoryReadAhead(const NtfsVolume<S>& volume)
    : impl_(std::make_unique<Impl>(volume)) {}

template <Cache::Strategy S>
DirectoryReadAhead<S>::~DirectoryReadAhead() = default;

template <Cache::Strategy S>
bool DirectoryReadAhead<S>::Enabled() const noexcept {
  return impl_->Enabled();
}

template <Cache::Strategy S>
std::size_t DirectoryReadAhead<S>::Window() const noexcept {
  return impl_->Window();
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Attach(FileRecord<S>& listing) {
  impl_->Attach(listing);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Begin() {
  impl_->Begin();
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Plan(std::span<const ULONGLONG> upcoming) {
  impl_->Plan(upcoming);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Release(ULONGLONG directory) {
  impl_->Release(directory);
}

template <Cache::Strategy S>
DirectoryReadAheadStats DirectoryReadAhead<S>::GetStats() const noexcept {
  return impl_->GetStats();
}

template class DirectoryReadAhead<Cache::Strategy::NoCache>;
template class DirectoryReadAhead<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
