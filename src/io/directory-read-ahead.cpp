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

// Most probe runs the plans of the held directories hold in all (a guard rail,
// not a tunable: a run costs 40 bytes, and 8 for each record it reads for). A
// directory whose runs do not fit is not queued on the standing lane, and its
// probes read as they did.
constexpr std::size_t max_planned_runs = std::size_t{1} << 16U;

// How many unread runs of a directory, from the first, a run that the probes
// have just read is looked for among. The probes read in the order of the plan,
// so the run is the first one or near it: a bound keeps a read that no plan
// names from costing the length of the plan.
constexpr std::size_t run_match_lookahead = 1024;

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
              volume.GetFileRecordSize()),
        lane_runs_(volume.GetMediaKind() == Tuning::MediaKind::Rotational
                       ? walk_.read_ahead_runs_rotational
                       : walk_.read_ahead_runs) {
    if constexpr (S == Cache::Strategy::FullCache) {
      overlap_slots_ = volume.GetFileRecordSize() == 0
                           ? 0
                           : cache_block_bytes_ / volume.GetFileRecordSize();
    }
  }

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

  // One read of $MFT that the probes of a held directory will make.
  struct PlannedRun {
    ULONGLONG first_slot;
    ULONGLONG count;
    // The refill that last put the run in the target.
    std::uint64_t stamp{0};
    // The records it reads for, as a span of RunPlan::wanted.
    std::uint32_t wanted_begin{0};
    std::uint32_t wanted_end{0};
    // Waiting: not on the lane. Announced: on it. Done: read, or not needed.
    enum class State : std::uint8_t { Waiting, Announced, Done };
    State state{State::Waiting};
  };

  // The probe runs of one held directory, in the order the walk reads them.
  struct RunPlan {
    std::vector<PlannedRun> runs;
    // The records the runs read for, run after run.
    std::vector<ULONGLONG> wanted;
    // The runs before it are Done.
    std::size_t cursor{0};
  };

  // A run the lane should hold, with the plan that owns it.
  struct Target {
    const RunPlan* plan;
    PlannedRun* run;
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

  // The batch of Plan(): reads ahead the records and blocks of the directories
  // of upcoming that are not held.
  void PlanBatch(std::span<const ULONGLONG> upcoming);

  // Whether the standing lane is in use: a count of runs, a budget that plans
  // something, and no raw callback.
  [[nodiscard]] bool LaneOn() const noexcept {
    return lane_runs_ != 0 && walk_.read_ahead_bytes != 0 &&
           !volume_->HasAttrRawCB();
  }

  // The bytes of record that a probe would find in the window or the memo,
  // without counting the use.
  [[nodiscard]] std::span<const BYTE> FindQuietly(ULONGLONG record);

  // Whether the reader could take the two runs for overlapping ranges, which
  // a withdrawal of one would then take with it.
  [[nodiscard]] bool Overlaps(const PlannedRun& left,
                              const PlannedRun& right) const noexcept;

  // Whether the window or the memo holds every record that run reads for.
  [[nodiscard]] bool HoldsRun(const RunPlan& plan, const PlannedRun& run);

  // Plans the probe runs of directory from the held part of its index, which
  // record holds as a file record that has been parsed. The blocks come from
  // the held blocks of directory.
  void PlanRunsOf(ULONGLONG directory, FileRecord<S>& record);

  // What the probes of a directory read, as they will cut it: for each group
  // of records, in order, the runs of each chunk.
  [[nodiscard]] RunPlan
      PlanOf(const std::vector<std::vector<ULONGLONG>>& groups);

  // Makes the standing lane hold the first lane_runs_ runs not read yet, the
  // rest of the directory being listed first, then order_ in order: announces
  // those that entered that set. A run that left it stays while it is among
  // the next lane_runs_: a stack that grows and shrinks pushes runs out of the
  // first and back in, and a withdrawal costs the read already made and the
  // read again. The others are withdrawn.
  void Refill();

  // The probes have read the run from first_slot on, count records long (the
  // `ran` of the listing's ProbeStore).
  void RunRead(ULONGLONG first_slot, ULONGLONG count);

  // Sends the hint for run to the reader: announces it on the standing lane,
  // or withdraws it. Never throws: a hint that fails costs the gain only.
  void SendHint(const PlannedRun& run, bool announce) noexcept;

  // Withdraws the runs of a plan that were announced, and forgets them.
  void WithdrawPlan(RunPlan& plan) noexcept;

  // Withdraws every announced run and forgets every plan.
  void WithdrawAll() noexcept;

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

  // Probe runs the standing lane keeps announced, at most: the knob that fits
  // the device. 0 is off.
  std::size_t lane_runs_;
  // The probe runs of each held directory that has any, by record number.
  std::unordered_map<ULONGLONG, RunPlan> plans_;
  // Runs in all in plans_.
  std::size_t planned_runs_{0};
  // Slots that two runs of a FullCache lane must be apart to be sure of
  // disjoint ranges: a block, since the reader's ranges are widened to blocks.
  // 0 under NoCache, whose ranges are the runs themselves.
  ULONGLONG overlap_slots_{0};
  // The runs on the lane now, in the order they were announced. They point
  // into plans_, which a plan leaves only through WithdrawPlan().
  std::vector<PlannedRun*> announced_;
  // The runs the lane should hold, in order, as Refill() works them out.
  std::vector<Target> target_;
  // The count of Refill() calls.
  std::uint64_t refills_{0};
  // The directories of the listing's stack that the lane looks at, the next
  // one first: the top of the stack at the last Plan(), then the directories
  // that were at the top before and are still planned, in their old order.
  std::vector<ULONGLONG> order_;
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
  // The three parts of the store work apart: a FullCache volume keeps no
  // window, and gets a store that only notes the runs read.
  typename FileRecord<S>::ProbeStore store;
  if (window_.Enabled() || memo_.Enabled()) {
    store.find = [this](ULONGLONG record) { return FindForProbe(record); };
    store.keep = [this](ULONGLONG first_record, std::span<const BYTE> bytes) {
      KeepRun(first_record, bytes);
    };
  }
  if (LaneOn()) {
    store.ran = [this](ULONGLONG first_slot, ULONGLONG count) {
      RunRead(first_slot, count);
    };
  }
  if (store.find || store.keep || store.ran) {
    listing.SetProbeStore(std::move(store));
  }
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Detach() noexcept {
  WithdrawAll();
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
  // A directory whose index sits in its record holds no bytes of its own, and
  // has a plan all the same.
  const auto plan = plans_.find(directory);
  if (plan != plans_.end()) {
    WithdrawPlan(plan->second);
    planned_runs_ -= plan->second.runs.size();
    plans_.erase(plan);
  }
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
  PlanBatch(upcoming);
  // The stack changed: the lane follows it. A directory that sank below the
  // window keeps its place under the new ones, and its runs stay announced.
  const std::size_t window = (std::min)(upcoming.size(), walk_.read_ahead_dirs);
  std::vector<ULONGLONG> order(
      upcoming.begin(), upcoming.begin() + gsl::narrow<std::ptrdiff_t>(window));
  const std::unordered_set<ULONGLONG> top(order.begin(), order.end());
  std::ranges::copy_if(
      order_, std::back_inserter(order), [this, &top](ULONGLONG directory) {
        return plans_.contains(directory) && !top.contains(directory);
      });
  order_ = std::move(order);
  Refill();
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::PlanBatch(
    std::span<const ULONGLONG> upcoming) {
  const DWORD record_size = volume_->GetFileRecordSize();

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
    // The index sits in the record: there is no block to read ahead, but the
    // probes of its entries are known.
    PlanRunsOf(*directory, *listing_);
    Refill();
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
  PlanRunsOf(*directory, *listing_);

  // The attributes already exist: they take their source from here on.
  for (const auto& attr : listing_->GetAttr(Attr::Type::IndexAllocation)) {
    static_cast<Attr::AttrIndexAlloc<S>*>(attr.get())
        ->SetBlockSource(
            [this](ULONGLONG owner, ULONGLONG vcn) {
              return HeldBlockBytes(owner, vcn);
            },
            *directory);
  }
  Refill();
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
      // Each owner holds the attributes of the candidate of its place.
      for (std::size_t index = 0; index < candidates.size(); index++) {
        PlanRunsOf(candidates.at(index).directory, *owners.at(index));
      }
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
      // No block to read, and the probes of its entries are known.
      PlanRunsOf(record, scratch);
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
std::span<const BYTE>
    DirectoryReadAhead<S>::Impl::FindQuietly(ULONGLONG record) {
  const std::span<const BYTE> bytes = window_.Find(record);
  return bytes.empty() ? memo_.Find(record) : bytes;
}

template <Cache::Strategy S>
bool DirectoryReadAhead<S>::Impl::Overlaps(
    const PlannedRun& left, const PlannedRun& right) const noexcept {
  // Each run, grown by overlap_slots_ on both sides.
  return left.first_slot < right.first_slot + right.count + overlap_slots_ &&
         right.first_slot < left.first_slot + left.count + overlap_slots_;
}

template <Cache::Strategy S>
bool DirectoryReadAhead<S>::Impl::HoldsRun(const RunPlan& plan,
                                           const PlannedRun& run) {
  if (!window_.Enabled() && !memo_.Enabled()) {
    return false;
  }
  const auto wanted =
      std::span<const ULONGLONG>(plan.wanted)
          .subspan(run.wanted_begin, run.wanted_end - run.wanted_begin);
  return std::ranges::none_of(
      wanted, [this](ULONGLONG record) { return FindQuietly(record).empty(); });
}

template <Cache::Strategy S>
typename DirectoryReadAhead<S>::Impl::RunPlan
    DirectoryReadAhead<S>::Impl::PlanOf(
        const std::vector<std::vector<ULONGLONG>>& groups) {
  // The store of the listing has a `keep` and a `find` exactly when the window
  // or the memo is on, and EntryProber plans from them.
  const bool keeps = window_.Enabled() || memo_.Enabled();
  const ProbeRunRules rules = ProbeRunRulesOf(*volume_, keeps);
  // The records that the earlier runs of this plan read: the window keeps each
  // run it is given, so the probes of a later group find them there.
  std::unordered_set<ULONGLONG> covered;
  const std::vector<BYTE> stand_in(rules.record_size);
  HeldRecordSource find;
  if (keeps) {
    find = [this, &covered, &stand_in](ULONGLONG record) {
      return covered.contains(record) ? std::span<const BYTE>(stand_in)
                                      : FindQuietly(record);
    };
  }
  const HeldRecordSink none;

  RunPlan plan;
  // What the prober of the walk has probed already: it probes a record once.
  std::unordered_set<ULONGLONG> known;
  std::vector<ULONGLONG> pending;
  const std::size_t round =
      (std::max<std::size_t>)(1, probe_.max_chunk_records);
  for (const std::vector<ULONGLONG>& group : groups) {
    if (known.size() >= probe_.max_known_records) {
      known.clear();
    }
    pending.clear();
    std::ranges::copy_if(
        group, std::back_inserter(pending),
        [&known](ULONGLONG record) { return known.insert(record).second; });
    std::ranges::sort(pending);
    for (std::size_t first = 0; first < pending.size(); first += round) {
      const std::span<const ULONGLONG> chunk =
          std::span<const ULONGLONG>(pending).subspan(
              first, (std::min)(round, pending.size() - first));
      const ProbeRunPlan chunk_plan = PlanProbeRuns(chunk, rules, find, none);
      for (const MftRun& run : chunk_plan.runs) {
        PlannedRun planned{.first_slot = run.first_slot,
                           .count = run.last_slot - run.first_slot + 1};
        planned.wanted_begin = gsl::narrow<std::uint32_t>(plan.wanted.size());
        std::ranges::copy(std::span<const ULONGLONG>(chunk_plan.to_read)
                              .subspan(run.begin, run.end - run.begin),
                          std::back_inserter(plan.wanted));
        planned.wanted_end = gsl::narrow<std::uint32_t>(plan.wanted.size());
        plan.runs.push_back(planned);
        if (keeps) {
          for (ULONGLONG slot = run.first_slot; slot <= run.last_slot; slot++) {
            covered.insert(slot);
          }
        }
      }
    }
  }
  return plan;
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::PlanRunsOf(ULONGLONG directory,
                                             FileRecord<S>& record) {
  if (!LaneOn() || plans_.contains(directory)) {
    return;
  }
  try {
    const auto& allocs = record.GetAttr(Attr::Type::IndexAllocation);
    auto* const alloc =
        allocs.empty()
            ? nullptr
            : static_cast<Attr::AttrIndexAlloc<S>*>(allocs.front().get());
    // The held blocks, without counting the listing as served.
    if (alloc != nullptr) {
      alloc->SetBlockSource(
          [this](ULONGLONG owner, ULONGLONG vcn) {
            return PeekBlockBytes(owner, vcn);
          },
          directory);
    }
    RunPlan plan = PlanOf(DirectoryIndex<S>(record).HeldProbeGroups());
    if (alloc != nullptr) {
      alloc->SetBlockSource({}, 0);
    }
    // A plan that does not fit is left out: the probes of that directory read
    // as they did.
    if (plan.runs.empty() ||
        planned_runs_ + plan.runs.size() > max_planned_runs) {
      return;
    }
    planned_runs_ += plan.runs.size();
    stats_.runs_planned += plan.runs.size();
    plans_.emplace(directory, std::move(plan));
  } catch (const std::exception&) {
    // A plan that fails costs the gain only.
  }
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::SendHint(const PlannedRun& run,
                                           bool announce) noexcept {
  try {
    // The hint of a forged slot MUST NOT log.
    log_.Reset(levels_);
    const Log::Detail::CaptureScope scope(log_);
    if (announce) {
      volume_->PrefetchMftSlotsStanding(run.first_slot, run.count);
    } else {
      volume_->CancelMftSlotsStanding(run.first_slot, run.count);
    }
  } catch (const std::exception&) {
    // A hint that fails costs the gain only.
  }
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::WithdrawPlan(RunPlan& plan) noexcept {
  for (PlannedRun& run : plan.runs) {
    if (run.state == PlannedRun::State::Announced) {
      SendHint(run, false);
      stats_.runs_withdrawn++;
    }
    run.state = PlannedRun::State::Done;
  }
  std::erase_if(announced_, [](const PlannedRun* run) {
    return run->state != PlannedRun::State::Announced;
  });
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::WithdrawAll() noexcept {
  for (const PlannedRun* const run : announced_) {
    SendHint(*run, false);
    stats_.runs_withdrawn++;
  }
  announced_.clear();
  target_.clear();
  plans_.clear();
  planned_runs_ = 0;
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::RunRead(ULONGLONG first_slot,
                                          ULONGLONG count) {
  if (!LaneOn() || listing_ == nullptr) {
    return;
  }
  const std::optional<ULONGLONG> directory = listing_->GetFileReference();
  const auto found = directory ? plans_.find(*directory) : plans_.end();
  if (found == plans_.end()) {
    stats_.runs_unplanned++;
    return;
  }
  RunPlan& plan = found->second;

  // The probes read in the order of the plan, so the run is the first one
  // that is not done, or near it, and is the plan's run or a part of it.
  const std::size_t end =
      (std::min)(plan.runs.size(), plan.cursor + run_match_lookahead);
  std::size_t index = plan.cursor;
  while (index < end) {
    const PlannedRun& run = plan.runs.at(index);
    if (run.state != PlannedRun::State::Done && run.first_slot <= first_slot &&
        first_slot + count <= run.first_slot + run.count) {
      break;
    }
    index++;
  }
  if (index == end) {
    stats_.runs_unplanned++;
    return;
  }

  // A run before it was not read: the probes found its records elsewhere, in
  // the window. The run itself is read, and what a smaller read left of it
  // is withdrawn. A FullCache reads by blocks and skips those its cache holds:
  // it may not reach the end of the range, which the reader then keeps.
  for (std::size_t skipped = plan.cursor; skipped <= index; skipped++) {
    PlannedRun& run = plan.runs.at(skipped);
    const bool whole = S == Cache::Strategy::NoCache && skipped == index &&
                       run.first_slot == first_slot && run.count == count;
    if (run.state == PlannedRun::State::Announced && !whole) {
      SendHint(run, false);
      // The run that was read is not counted as withdrawn unread.
      stats_.runs_withdrawn += skipped == index ? 0 : 1;
    }
    run.state = PlannedRun::State::Done;
  }
  stats_.runs_read++;
  std::erase_if(announced_, [](const PlannedRun* run) {
    return run->state != PlannedRun::State::Announced;
  });
  plan.cursor = index + 1;
  while (plan.cursor < plan.runs.size() &&
         plan.runs.at(plan.cursor).state == PlannedRun::State::Done) {
    plan.cursor++;
  }
  Refill();
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Refill() {
  if (!LaneOn() || plans_.empty()) {
    return;
  }
  refills_++;

  // The first lane_runs_ runs not read yet: the rest of the directory being
  // listed, then the directories of order_. The next lane_runs_ stay on the
  // lane when they are there, and are not announced.
  target_.clear();
  std::size_t room = lane_runs_;
  std::size_t keep_room = lane_runs_;
  const auto take = [this, &room, &keep_room](ULONGLONG directory) {
    const auto found = plans_.find(directory);
    if (found == plans_.end()) {
      return;
    }
    RunPlan& plan = found->second;
    for (std::size_t index = plan.cursor;
         index < plan.runs.size() && (room > 0 || keep_room > 0); index++) {
      PlannedRun& run = plan.runs.at(index);
      if (run.state == PlannedRun::State::Done) {
        continue;
      }
      run.stamp = refills_;
      if (room > 0) {
        target_.push_back({.plan = &plan, .run = &run});
        room--;
      } else {
        keep_room--;
      }
    }
  };
  std::optional<ULONGLONG> current;
  if (listing_ != nullptr) {
    current = listing_->GetFileReference();
  }
  if (current) {
    take(*current);
  }
  for (const ULONGLONG directory : order_) {
    if (room == 0 && keep_room == 0) {
      break;
    }
    if (directory != current) {
      take(directory);
    }
  }

  // The runs that left the target are withdrawn, and may come back.
  for (PlannedRun* const run : announced_) {
    if (run->stamp != refills_) {
      SendHint(*run, false);
      run->state = PlannedRun::State::Waiting;
      stats_.runs_withdrawn++;
    }
  }
  std::erase_if(announced_, [](const PlannedRun* run) {
    return run->state != PlannedRun::State::Announced;
  });

  // The runs that entered it are announced, but for those whose records the
  // window holds by now, and for those that overlap a run on the lane: a
  // withdrawal takes every range that overlaps it, so the ranges on the lane
  // are kept apart. Such a run waits until the other one is read.
  for (const Target& entry : target_) {
    PlannedRun& run = *entry.run;
    if (run.state != PlannedRun::State::Waiting) {
      continue;
    }
    if (HoldsRun(*entry.plan, run)) {
      run.state = PlannedRun::State::Done;
      stats_.runs_window_held++;
      continue;
    }
    if (std::ranges::any_of(announced_, [this, &run](const PlannedRun* other) {
          return Overlaps(*other, run);
        })) {
      continue;
    }
    SendHint(run, true);
    run.state = PlannedRun::State::Announced;
    announced_.push_back(&run);
    stats_.runs_announced++;
  }
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
