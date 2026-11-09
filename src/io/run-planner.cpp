#include "io/run-planner.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "attr/base.h"
#include "attr/index-alloc.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "io/file-record-index.h"
#include "io/file-record.h"
#include "io/mft-runs.h"
#include "io/read-ahead-held.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// A set of record numbers in one flat table, which allocates when it grows and
// not once for each record. PlanOf() inserts millions of them in all.
class RecordSet {
 public:
  [[nodiscard]] std::size_t Size() const noexcept { return size_; }

  // Empties the set. The table keeps its size.
  void Clear() noexcept {
    std::ranges::fill(slots_, vacant);
    size_ = 0;
    holds_vacant_ = false;
  }

  // Adds record. Returns false when the set held it already.
  bool Insert(ULONGLONG record) {
    // The mark of a free slot is a number too: it is kept apart.
    if (record == vacant) {
      if (holds_vacant_) {
        return false;
      }
      holds_vacant_ = true;
      size_++;
      return true;
    }
    // At most half of the table is used, so a probe always ends.
    if ((size_ + 1) * 2 > slots_.size()) {
      Grow();
    }
    const std::size_t at = SlotOf(record);
    if (slots_[at] == record) {
      return false;
    }
    slots_[at] = record;
    size_++;
    return true;
  }

 private:
  // The mark of a free slot. A record number has 48 bits, so only a forged one
  // can equal it.
  static constexpr ULONGLONG vacant = ~ULONGLONG{0};

  // Slots a table starts with, a power of two.
  static constexpr std::size_t first_capacity = 64;

  // The slot that holds record, or the free one where it would go.
#ifdef __clang__
  // The product wraps on purpose: the hash keeps its high bits mod 2^64.
  __attribute__((no_sanitize("unsigned-integer-overflow")))
#endif
  [[nodiscard]] std::size_t SlotOf(ULONGLONG record) const noexcept {
    // 2^64 divided by the golden ratio: it spreads the low bits of a record
    // number, which are all that differs between the records of a directory.
    constexpr ULONGLONG golden = 0x9E3779B97F4A7C15ULL;
    const std::size_t mask = slots_.size() - 1;
    std::size_t at = static_cast<std::size_t>((record * golden) >> 32U) & mask;
    while (slots_[at] != vacant && slots_[at] != record) {
      at = (at + 1) & mask;
    }
    return at;
  }

  // Doubles the table and places the records again.
  void Grow() {
    std::vector<ULONGLONG> old = std::move(slots_);
    slots_.assign((std::max)(first_capacity, old.size() * 2), vacant);
    for (const ULONGLONG record : old) {
      if (record != vacant) {
        slots_[SlotOf(record)] = record;
      }
    }
  }

  std::vector<ULONGLONG> slots_;
  std::size_t size_{0};
  bool holds_vacant_{false};
};

// The slots first to last of $MFT, both included.
struct SlotInterval {
  ULONGLONG first;
  ULONGLONG last;
};

// The slots of $MFT that the runs of a plan cover, as sorted intervals that
// neither overlap nor touch. A run covers up to a thousand slots, so a set of
// the slots cost one allocation for each of them.
class SlotCover {
 public:
  // Whether slot lies in a run added so far.
  [[nodiscard]] bool Contains(ULONGLONG slot) const noexcept {
    const auto after = std::ranges::upper_bound(
        intervals_, slot, {},
        [](const SlotInterval& span) { return span.first; });
    return after != intervals_.begin() && std::prev(after)->last >= slot;
  }

  // Adds runs, in any order. They are left sorted.
  void Add(std::vector<SlotInterval>& runs) {
    if (runs.empty()) {
      return;
    }
    const auto by_first = [](const SlotInterval& left,
                             const SlotInterval& right) {
      return left.first < right.first;
    };
    std::ranges::sort(runs, by_first);
    merged_.clear();
    std::ranges::merge(intervals_, runs, std::back_inserter(merged_), by_first);
    intervals_.clear();
    for (const SlotInterval& next : merged_) {
      // The next one overlaps the last kept, or starts right after it.
      if (!intervals_.empty() && (next.first <= intervals_.back().last ||
                                  next.first - intervals_.back().last == 1)) {
        intervals_.back().last = (std::max)(intervals_.back().last, next.last);
      } else {
        intervals_.push_back(next);
      }
    }
  }

 private:
  std::vector<SlotInterval> intervals_;
  // The merge of intervals_ and the runs being added, kept for its room.
  std::vector<SlotInterval> merged_;
};

// Puts into pending, sorted, the records of group that known has not met, and
// notes them. The prober of the walk probes a record once, so known forgets
// everything when it holds max_known records.
void TakeUnprobed(const std::vector<ULONGLONG>& group, RecordSet& known,
                  std::size_t max_known, std::vector<ULONGLONG>& pending) {
  if (known.Size() >= max_known) {
    known.Clear();
  }
  pending.clear();
  std::ranges::copy_if(
      group, std::back_inserter(pending),
      [&known](ULONGLONG record) { return known.Insert(record); });
  std::ranges::sort(pending);
}

// Adds to fresh the slots that the runs of chunk_plan read.
void NoteCovered(const ProbeRunPlan& chunk_plan,
                 std::vector<SlotInterval>& fresh) {
  for (const MftRun& run : chunk_plan.runs) {
    if (run.first_slot <= run.last_slot) {
      fresh.push_back({.first = run.first_slot, .last = run.last_slot});
    }
  }
}

}  // namespace

template <Cache::Strategy S>
RunPlanner<S>::RunPlanner(const NtfsVolume<S>& volume, RecordKeeper& keeper,
                          const HeldStore& store)
    : volume_(&volume),
      keeper_(&keeper),
      store_(&store),
      probe_(volume.GetOptions().tuning.probe) {}

template <Cache::Strategy S>
void RunPlanner<S>::AppendRuns(RunPlan& plan, const ProbeRunPlan& chunk_plan) {
  for (const MftRun& run : chunk_plan.runs) {
    PlannedRun planned{.first_slot = run.first_slot,
                       .count = run.last_slot - run.first_slot + 1};
    planned.wanted_begin = gsl::narrow<std::uint32_t>(plan.wanted.size());
    std::ranges::copy(std::span<const ULONGLONG>(chunk_plan.to_read)
                          .subspan(run.begin, run.end - run.begin),
                      std::back_inserter(plan.wanted));
    planned.wanted_end = gsl::narrow<std::uint32_t>(plan.wanted.size());
    plan.runs.push_back(planned);
  }
}

template <Cache::Strategy S>
RunPlan
    RunPlanner<S>::PlanOf(const std::vector<std::vector<ULONGLONG>>& groups) {
  // The store of the listing has a `keep` and a `find` exactly when the window
  // or the memo is on, and EntryProber plans from them.
  const bool keeps = keeper_->Enabled();
  const ProbeRunRules rules = ProbeRunRulesOf(*volume_, keeps);
  // The records that the earlier runs of this plan read: the window keeps each
  // run it is given, so the probes of a later group find them there.
  SlotCover covered;
  std::vector<SlotInterval> fresh;
  const std::vector<BYTE> stand_in(rules.record_size);
  HeldRecordSource find;
  if (keeps) {
    find = [this, &covered, &stand_in](ULONGLONG record) {
      return covered.Contains(record) ? std::span<const BYTE>(stand_in)
                                      : keeper_->FindQuietly(record);
    };
  }
  const HeldRecordSink none;

  RunPlan plan;
  // What the prober of the walk has probed already: it probes a record once.
  RecordSet known;
  std::vector<ULONGLONG> pending;
  const std::size_t round =
      (std::max<std::size_t>)(1, probe_.max_chunk_records);
  for (const std::vector<ULONGLONG>& group : groups) {
    TakeUnprobed(group, known, probe_.max_known_records, pending);
    for (std::size_t first = 0; first < pending.size(); first += round) {
      const std::span<const ULONGLONG> chunk =
          std::span<const ULONGLONG>(pending).subspan(
              first, (std::min)(round, pending.size() - first));
      const ProbeRunPlan chunk_plan = PlanProbeRuns(chunk, rules, find, none);
      AppendRuns(plan, chunk_plan);
      if (keeps) {
        NoteCovered(chunk_plan, fresh);
      }
      // The runs of a chunk count for the next chunk, not for its own.
      covered.Add(fresh);
      fresh.clear();
    }
  }
  return plan;
}

template <Cache::Strategy S>
RunPlan RunPlanner<S>::PlanFor(ULONGLONG directory, FileRecord<S>& record) {
  const auto& allocs = record.GetAttr(Attr::Type::IndexAllocation);
  auto* const alloc =
      allocs.empty()
          ? nullptr
          : static_cast<Attr::AttrIndexAlloc<S>*>(allocs.front().get());
  // The held blocks, without counting the listing as served.
  if (alloc != nullptr) {
    alloc->SetBlockSource(
        [this](ULONGLONG owner, ULONGLONG vcn) {
          return store_->PeekBlock(owner, vcn);
        },
        directory);
  }
  RunPlan plan = PlanOf(DirectoryIndex<S>(record).HeldProbeGroups());
  if (alloc != nullptr) {
    alloc->SetBlockSource({}, 0);
  }
  return plan;
}

template class RunPlanner<Cache::Strategy::NoCache>;
template class RunPlanner<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
