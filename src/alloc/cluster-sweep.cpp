#include "alloc/cluster-sweep.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc/claim-inventory.h"
#include "alloc/content-check.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// Bytes one read of the content check covers at most: 4 MiB, as large as a
// window of $MFT, so that a disk reads at its speed and the buffer stays small.
constexpr ULONGLONG max_check_read_bytes = ULONGLONG{4} * 1024 * 1024;

// Bytes of clusters nobody asks for that one read of the content check may
// span between two it wants: a seek costs more than reading 64 KiB.
constexpr ULONGLONG max_check_gap_bytes = ULONGLONG{64} * 1024;

// Reads the content check has announced to the reader and not made yet, at
// most. The reader keeps 32 reads in flight.
constexpr std::size_t max_announced_check_reads = 32;

// Bytes the content check announces ahead of the read it makes: 32 MiB, half
// of what the reader keeps in flight.
constexpr ULONGLONG max_announced_check_bytes = ULONGLONG{32} * 1024 * 1024;

// A stretch of one stream that the sweep settled: [vcn, vcn + clusters) at lcn
// is in one condition. 32 bytes.
struct Resolved {
  ULONGLONG lcn{0};
  ULONGLONG vcn{0};
  std::uint32_t clusters{0};
  // Index of the account stream in the inventory.
  std::uint32_t account{0};
  ClusterCondition condition{ClusterCondition::Unknown};
};

// A claim that covers the stretch being judged.
struct Active {
  ULONGLONG lcn{0};
  ULONGLONG vcn{0};
  ULONGLONG end{0};
  ULONGLONG lsn{0};
  std::uint32_t owner{0};
  std::uint32_t account{no_account};
  bool freed{false};
  bool unwritten{false};
};

// The condition of a cluster of a freed stream, claimed by claim, while the
// claims in active cover it and the bitmap says state.
ClusterCondition FreedCondition(const Active& claim,
                                std::span<const Active> active,
                                ClusterState state) noexcept {
  if (state == ClusterState::OutOfRange) {
    return ClusterCondition::Invalid;
  }
  if (state == ClusterState::Bad) {
    return ClusterCondition::Bad;
  }
  if (claim.unwritten) {
    return ClusterCondition::NeverWritten;
  }

  bool live_written = false;
  bool live_any = false;
  bool freed_later = false;
  bool freed_unordered = false;
  for (const Active& other : active) {
    if (other.owner == claim.owner) {
      continue;
    }
    if (!other.freed) {
      live_any = true;
      live_written = live_written || !other.unwritten;
    } else if (!other.unwritten) {
      // Without two sequence numbers that differ, nothing orders the two.
      if (other.lsn == claim.lsn || other.lsn == 0 || claim.lsn == 0) {
        freed_unordered = true;
      } else if (other.lsn > claim.lsn) {
        freed_later = true;
      }
    }
  }
  if (live_written) {
    return ClusterCondition::ReallocatedWritten;
  }
  if (live_any) {
    return ClusterCondition::ReallocatedUnwritten;
  }
  if (freed_later) {
    return ClusterCondition::OverwrittenThenFreed;
  }
  if (freed_unordered) {
    return ClusterCondition::Contested;
  }
  if (state == ClusterState::Allocated) {
    return ClusterCondition::AllocatedUnknownOwner;
  }
  if (state == ClusterState::Unknown) {
    return ClusterCondition::Unknown;
  }
  return ClusterCondition::Intact;
}

// The condition of a cluster of a live stream, as FreedCondition() does.
ClusterCondition LiveCondition(const Active& claim,
                               std::span<const Active> active,
                               ClusterState state) noexcept {
  if (state == ClusterState::OutOfRange) {
    return ClusterCondition::Invalid;
  }
  if (state == ClusterState::Bad) {
    return ClusterCondition::Bad;
  }
  const bool cross_linked =
      std::ranges::any_of(active, [&claim](const Active& other) {
        return other.owner != claim.owner && !other.freed;
      });
  if (cross_linked) {
    return ClusterCondition::CrossLinked;
  }
  if (state == ClusterState::Free) {
    return ClusterCondition::FreeButClaimed;
  }
  if (state == ClusterState::Unknown) {
    return ClusterCondition::Unknown;
  }
  return ClusterCondition::Consistent;
}

// The sweep in LCN order: it turns the sorted claims into Resolved stretches.
template <Cache::Strategy S>
class Sweeper {
 public:
  Sweeper(const NtfsVolume<S>& volume, const ClaimInventory& inventory)
      : volume_(&volume), inventory_(&inventory) {}

  // Judges every stretch of every concerned stream. The inventory's claims MUST
  // be sorted by lcn.
  std::vector<Resolved> Run() {
    const std::vector<Claim>& claims = inventory_->claims;
    std::size_t next = 0;
    ULONGLONG pos = 0;
    while (next < claims.size() || !active_.empty()) {
      if (active_.empty()) {
        pos = claims.at(next).lcn;
      }
      while (next < claims.size() && claims.at(next).lcn <= pos) {
        Enter(claims.at(next));
        next++;
      }
      ULONGLONG end = (std::numeric_limits<ULONGLONG>::max)();
      for (const Active& active : active_) {
        end = (std::min)(end, active.end);
      }
      if (next < claims.size()) {
        end = (std::min)(end, claims.at(next).lcn);
      }
      Judge(pos, end);
      pos = end;
      std::erase_if(active_,
                    [pos](const Active& active) { return active.end <= pos; });
    }
    return std::move(resolved_);
  }

 private:
  // Adds claim to the claims that cover the current position.
  void Enter(const Claim& claim) {
    const ClaimInventory::StreamRef& stream =
        inventory_->streams.at(claim.stream);
    const ClaimInventory::Owner& owner = inventory_->owners.at(stream.owner);
    active_.push_back({.lcn = claim.lcn,
                       .vcn = claim.vcn,
                       .end = claim.lcn + claim.Clusters(),
                       .lsn = owner.lsn,
                       .owner = stream.owner,
                       .account = stream.account,
                       .freed = owner.freed,
                       .unwritten = claim.Unwritten()});
  }

  // Judges [pos, end), over which the set of claims is constant, for each
  // concerned claim. The bitmap is asked only when one needs it.
  void Judge(ULONGLONG pos, ULONGLONG end) {
    if (std::ranges::none_of(active_, [](const Active& active) {
          return active.account != no_account;
        })) {
      return;
    }
    volume_->GetClusterRuns(pos, end - pos, runs_);
    for (const Active& claim : active_) {
      if (claim.account == no_account) {
        continue;
      }
      std::optional<std::size_t> last;
      for (const ClusterRun& run : runs_) {
        const ClusterCondition condition =
            claim.freed ? FreedCondition(claim, active_, run.state)
                        : LiveCondition(claim, active_, run.state);
        if (last && resolved_.at(*last).condition == condition &&
            resolved_.at(*last).lcn + resolved_.at(*last).clusters == run.lcn) {
          resolved_.at(*last).clusters +=
              static_cast<std::uint32_t>(run.clusters);
          continue;
        }
        resolved_.push_back(
            {.lcn = run.lcn,
             .vcn = claim.vcn + (run.lcn - claim.lcn),
             .clusters = static_cast<std::uint32_t>(run.clusters),
             .account = claim.account,
             .condition = condition});
        last = resolved_.size() - 1;
      }
    }
  }

  const NtfsVolume<S>* volume_;
  const ClaimInventory* inventory_;
  std::vector<Active> active_;
  std::vector<ClusterRun> runs_;
  std::vector<Resolved> resolved_;
};

// A part of one Resolved stretch that the content check looks at.
struct CheckTarget {
  ULONGLONG lcn{0};
  ULONGLONG clusters{0};
  std::size_t index{0};
  // Clusters between the start of the stretch and this part.
  ULONGLONG offset{0};
};

// What the content check found for clusters of one stretch.
struct CheckEdit {
  std::size_t index{0};
  ULONGLONG offset{0};
  ULONGLONG clusters{0};
  ClusterCondition condition{ClusterCondition::IntactChecked};
};

// The reads of the content check: contiguous targets, cut so that none passes
// max_check_read_bytes, and merged while the gap between them stays small.
struct CheckRead {
  std::size_t first_target{0};
  std::size_t target_count{0};
  ULONGLONG lcn{0};
  ULONGLONG clusters{0};
};

// The content check of the clusters presumed intact: reads them in ascending
// order, announced to the reader ahead of the reads, and tells which are
// zeros, which are NTFS metadata and which look like data.
template <Cache::Strategy S>
class ContentChecker {
 public:
  ContentChecker(const NtfsVolume<S>& volume, const ClaimInventory& inventory,
                 DataCheckMode mode)
      : volume_(&volume),
        inventory_(&inventory),
        mode_(mode),
        cluster_size_(volume.GetClusterSize()) {}

  // Replaces the Intact stretches of resolved that the check could read by the
  // stretches it found, and returns how many clusters it read.
  ULONGLONG Run(std::vector<Resolved>& resolved) {
    if (cluster_size_ == 0) {
      return 0;
    }
    std::vector<CheckTarget> targets = ChooseTargets(resolved);
    if (targets.empty()) {
      return 0;
    }
    std::ranges::sort(targets, [](const CheckTarget& left,
                                  const CheckTarget& right) {
      return std::tie(left.lcn, left.index) < std::tie(right.lcn, right.index);
    });
    const std::vector<CheckRead> reads = PlanReads(targets);
    std::vector<CheckEdit> edits;
    const ULONGLONG clusters_read = ReadAll(targets, reads, edits);
    Apply(resolved, edits);
    return clusters_read;
  }

 private:
  // The parts of the Intact stretches to read. An encrypted stream is left
  // alone: its ciphertext proves nothing. A sample is the first cluster of the
  // stretch with the lowest VCN of each stream.
  std::vector<CheckTarget>
      ChooseTargets(const std::vector<Resolved>& resolved) {
    std::vector<CheckTarget> targets;
    std::unordered_map<std::uint32_t, std::size_t> lowest;
    for (std::size_t i = 0; i < resolved.size(); i++) {
      const Resolved& stretch = resolved.at(i);
      if (stretch.condition != ClusterCondition::Intact ||
          inventory_->accounts.at(stretch.account).encrypted) {
        continue;
      }
      if (mode_ == DataCheckMode::Full) {
        Cut(targets, stretch, i);
        continue;
      }
      const auto [found, added] = lowest.try_emplace(stretch.account, i);
      if (!added && resolved.at(found->second).vcn > stretch.vcn) {
        found->second = i;
      }
    }
    if (mode_ == DataCheckMode::Sample) {
      for (const auto& [account, index] : lowest) {
        const Resolved& stretch = resolved.at(index);
        targets.push_back(
            {.lcn = stretch.lcn, .clusters = 1, .index = index, .offset = 0});
      }
    }
    return targets;
  }

  // Appends stretch as targets no longer than one read.
  void Cut(std::vector<CheckTarget>& targets, const Resolved& stretch,
           std::size_t index) const {
    const ULONGLONG per_read =
        (std::max)(ULONGLONG{1}, max_check_read_bytes / cluster_size_);
    for (ULONGLONG done = 0; done < stretch.clusters; done += per_read) {
      targets.push_back(
          {.lcn = stretch.lcn + done,
           .clusters = (std::min)(per_read, stretch.clusters - done),
           .index = index,
           .offset = done});
    }
  }

  // Groups the sorted targets into reads.
  std::vector<CheckRead>
      PlanReads(const std::vector<CheckTarget>& targets) const {
    const ULONGLONG max_clusters =
        (std::max)(ULONGLONG{1}, max_check_read_bytes / cluster_size_);
    const ULONGLONG max_gap = max_check_gap_bytes / cluster_size_;
    std::vector<CheckRead> reads;
    for (std::size_t i = 0; i < targets.size(); i++) {
      const CheckTarget& target = targets.at(i);
      const ULONGLONG target_end = target.lcn + target.clusters;
      if (!reads.empty()) {
        CheckRead& last = reads.back();
        const ULONGLONG last_end = last.lcn + last.clusters;
        const ULONGLONG new_end = (std::max)(last_end, target_end);
        if (target.lcn <= last_end + max_gap &&
            new_end - last.lcn <= max_clusters) {
          last.clusters = new_end - last.lcn;
          last.target_count++;
          continue;
        }
      }
      reads.push_back({.first_target = i,
                       .target_count = 1,
                       .lcn = target.lcn,
                       .clusters = target.clusters});
    }
    return reads;
  }

  // Announces the reads from announced on that fit the window ahead of read
  // number current, and returns the next one to announce.
  std::size_t Announce(const std::vector<CheckRead>& reads, std::size_t current,
                       std::size_t announced) const {
    ULONGLONG bytes = 0;
    for (std::size_t i = current; i < announced; i++) {
      bytes += reads.at(i).clusters * cluster_size_;
    }
    while (announced < reads.size() &&
           announced - current < max_announced_check_reads &&
           bytes < max_announced_check_bytes) {
      const CheckRead& read = reads.at(announced);
      volume_->Prefetch(read.lcn * cluster_size_,
                        read.clusters * cluster_size_);
      bytes += read.clusters * cluster_size_;
      announced++;
    }
    return announced;
  }

  // Makes the reads, in order, and classifies every cluster of every target.
  ULONGLONG ReadAll(const std::vector<CheckTarget>& targets,
                    const std::vector<CheckRead>& reads,
                    std::vector<CheckEdit>& edits) {
    // A byte address MUST fit a LONGLONG.
    const ULONGLONG max_lcn =
        static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)()) /
        cluster_size_;
    std::vector<BYTE> buffer;
    ULONGLONG clusters_read = 0;
    std::size_t announced = 0;
    for (std::size_t r = 0; r < reads.size(); r++) {
      announced = Announce(reads, r, announced);
      const CheckRead& read = reads.at(r);
      if (read.lcn > max_lcn || read.clusters > max_lcn - read.lcn) {
        continue;
      }
      buffer.resize(static_cast<std::size_t>(read.clusters * cluster_size_));
      LARGE_INTEGER addr{};
      addr.QuadPart = static_cast<LONGLONG>(read.lcn * cluster_size_);
      if (!volume_->ReadInto(addr, buffer)) {
        Log::Debug("Content check: the {} clusters at LCN {} cannot be read",
                   read.clusters, read.lcn);
        continue;
      }
      clusters_read += read.clusters;
      for (std::size_t t = read.first_target;
           t < read.first_target + read.target_count; t++) {
        Classify(targets.at(t), read.lcn, buffer, edits);
      }
    }
    volume_->CancelPrefetch();
    return clusters_read;
  }

  // Classifies the clusters of target, which buffer holds from first_lcn on,
  // and appends one edit per run of clusters that look alike.
  void Classify(const CheckTarget& target, ULONGLONG first_lcn,
                std::span<const BYTE> buffer,
                std::vector<CheckEdit>& edits) const {
    std::optional<ClusterCondition> run_condition;
    ULONGLONG run_start = 0;
    const auto close = [&](ULONGLONG end) {
      if (run_condition) {
        edits.push_back({.index = target.index,
                         .offset = target.offset + run_start,
                         .clusters = end - run_start,
                         .condition = *run_condition});
      }
    };
    for (ULONGLONG c = 0; c < target.clusters; c++) {
      const std::size_t at = static_cast<std::size_t>(
          (target.lcn + c - first_lcn) * cluster_size_);
      ClusterCondition condition = ClusterCondition::IntactChecked;
      switch (ClassifyCluster(
          buffer.subspan(at, static_cast<std::size_t>(cluster_size_)),
          volume_->GetSectorSize())) {
        case ClusterContent::Zeros:
          condition = ClusterCondition::Zeroed;
          break;
        case ClusterContent::Metadata:
          condition = ClusterCondition::OverwrittenByMetadata;
          break;
        case ClusterContent::Data:
          break;
      }
      if (run_condition != condition) {
        close(c);
        run_condition = condition;
        run_start = c;
      }
    }
    close(target.clusters);
  }

  // Splits the stretches that edits touch, and gives the clusters of each edit
  // its condition. The others stay as they were.
  static void Apply(std::vector<Resolved>& resolved,
                    std::vector<CheckEdit>& edits) {
    std::ranges::sort(edits, [](const CheckEdit& left, const CheckEdit& right) {
      return std::tie(left.index, left.offset) <
             std::tie(right.index, right.offset);
    });
    std::vector<Resolved> result;
    result.reserve(resolved.size() + edits.size() * 2);
    std::size_t edit = 0;
    for (std::size_t i = 0; i < resolved.size(); i++) {
      const Resolved& stretch = resolved.at(i);
      ULONGLONG done = 0;
      const auto emit = [&](ULONGLONG offset, ULONGLONG clusters,
                            ClusterCondition condition) {
        if (clusters != 0) {
          result.push_back({.lcn = stretch.lcn + offset,
                            .vcn = stretch.vcn + offset,
                            .clusters = static_cast<std::uint32_t>(clusters),
                            .account = stretch.account,
                            .condition = condition});
        }
      };
      for (; edit < edits.size() && edits.at(edit).index == i; edit++) {
        const CheckEdit& found = edits.at(edit);
        emit(done, found.offset - done, stretch.condition);
        emit(found.offset, found.clusters, found.condition);
        done = found.offset + found.clusters;
      }
      emit(done, stretch.clusters - done, stretch.condition);
    }
    resolved = std::move(result);
  }

  const NtfsVolume<S>* volume_;
  const ClaimInventory* inventory_;
  DataCheckMode mode_;
  ULONGLONG cluster_size_;
};

// Whether a cluster of this condition keeps the compression unit it sits in
// from decompressing.
bool BreaksUnit(ClusterCondition condition) noexcept {
  return IsDamaged(condition);
}

// Whether a cluster of this condition is lost with its unit when the unit
// breaks: it was fine, or it was a hole of the unit.
bool LostWhenUnitBreaks(ClusterCondition condition) noexcept {
  return condition == ClusterCondition::Intact ||
         condition == ClusterCondition::IntactChecked ||
         condition == ClusterCondition::Consistent ||
         condition == ClusterCondition::Sparse;
}

// The units of a compressed stream that hold a cluster in a state that breaks
// them, as ranges of VCNs, sorted and merged. pieces MUST be sorted by VCN.
std::vector<VcnRange> BrokenUnits(const std::vector<ClusterPiece>& pieces,
                                  ULONGLONG unit) {
  std::vector<VcnRange> broken;
  for (const ClusterPiece& piece : pieces) {
    if (!BreaksUnit(piece.condition) || piece.clusters == 0) {
      continue;
    }
    const ULONGLONG first = piece.first_vcn / unit * unit;
    const ULONGLONG last_vcn = piece.first_vcn + piece.clusters - 1;
    const ULONGLONG end = last_vcn / unit * unit + unit;
    if (!broken.empty() &&
        first <= broken.back().vcn + broken.back().clusters) {
      const ULONGLONG joined_end =
          (std::max)(end, broken.back().vcn + broken.back().clusters);
      broken.back().clusters = joined_end - broken.back().vcn;
      continue;
    }
    broken.push_back({.vcn = first, .clusters = end - first});
  }
  return broken;
}

// Splits pieces, sorted by VCN, at the edges of the broken ranges, and marks
// as lost with their unit the parts inside them that were fine.
std::vector<ClusterPiece>
    MarkBrokenUnits(const std::vector<ClusterPiece>& pieces,
                    const std::vector<VcnRange>& broken) {
  std::vector<ClusterPiece> result;
  result.reserve(pieces.size());
  std::size_t at = 0;
  for (const ClusterPiece& piece : pieces) {
    ULONGLONG vcn = piece.first_vcn;
    const ULONGLONG end = piece.first_vcn + piece.clusters;
    const auto emit = [&](ULONGLONG from, ULONGLONG to, bool inside) {
      if (to <= from) {
        return;
      }
      ClusterPiece part = piece;
      part.first_vcn = from;
      part.clusters = to - from;
      if (part.lcn) {
        part.lcn = *piece.lcn + (from - piece.first_vcn);
      }
      if (inside && LostWhenUnitBreaks(piece.condition)) {
        part.condition = ClusterCondition::LostWithUnit;
      }
      result.push_back(part);
    };
    while (at < broken.size() &&
           broken.at(at).vcn + broken.at(at).clusters <= vcn) {
      at++;
    }
    for (std::size_t i = at; i < broken.size() && vcn < end; i++) {
      const ULONGLONG from = broken.at(i).vcn;
      const ULONGLONG to = from + broken.at(i).clusters;
      if (from >= end) {
        break;
      }
      emit(vcn, (std::min)(from, end), false);
      vcn = (std::max)(vcn, from);
      emit(vcn, (std::min)(to, end), true);
      vcn = (std::min)(to, end);
    }
    emit(vcn, end, false);
  }
  return result;
}

// Builds the account of every concerned record from the stretches the sweep
// settled, and from what each stream says of its holes and invalid ranges.
class Assembler {
 public:
  Assembler(const ClaimInventory& inventory, ClusterAccountLevel level)
      : inventory_(&inventory),
        level_(level),
        results_(inventory.concerned_records.size()) {
    if (level == ClusterAccountLevel::CountsAndMap) {
      pieces_.resize(inventory.accounts.size());
    }
  }

  // Counts a settled stretch, or keeps it as a piece of its stream's map.
  void Add(const Resolved& stretch) {
    const AccountStream& stream = inventory_->accounts.at(stretch.account);
    // A compressed stream is counted from its pieces, once its broken units
    // are known.
    if (stream.unit_clusters == 0) {
      results_.at(stream.owner_dense)
          .tally.Add(stretch.condition, stretch.clusters);
    }
    if (!NeedsPieces(stream)) {
      return;
    }
    PiecesOf(stretch.account)
        .push_back({.first_vcn = stretch.vcn,
                    .clusters = stretch.clusters,
                    .lcn = stretch.lcn,
                    .condition = stretch.condition});
  }

  // Finishes every stream, and returns the accounts of the records that have
  // clusters to count.
  std::vector<OwnerAccount> Finish() {
    for (std::size_t i = 0; i < inventory_->accounts.size(); i++) {
      FinishStream(static_cast<std::uint32_t>(i));
    }
    std::vector<OwnerAccount> accounts;
    for (std::size_t i = 0; i < results_.size(); i++) {
      ClusterAccount& account = results_.at(i);
      if (account.tally.Empty() && account.streams.empty()) {
        continue;
      }
      accounts.push_back({.record = inventory_->concerned_records.at(i),
                          .account = std::move(account)});
    }
    return accounts;
  }

 private:
  // Whether a stream keeps its pieces: the map wants them, and so does the
  // marking of the units of a compressed one.
  [[nodiscard]] bool NeedsPieces(const AccountStream& stream) const noexcept {
    return level_ == ClusterAccountLevel::CountsAndMap ||
           stream.unit_clusters != 0;
  }

  std::vector<ClusterPiece>& PiecesOf(std::uint32_t account) {
    if (level_ == ClusterAccountLevel::CountsAndMap) {
      return pieces_.at(account);
    }
    return kept_[account];
  }

  // Adds the holes and invalid ranges of stream to the tally, and builds its
  // map when the level wants one.
  void FinishStream(std::uint32_t account) {
    const AccountStream& stream = inventory_->accounts.at(account);
    ClusterAccount& result = results_.at(stream.owner_dense);

    ULONGLONG sparse = 0;
    for (const VcnRange& range : stream.sparse) {
      sparse += range.clusters;
    }
    ULONGLONG invalid = 0;
    for (const VcnRange& range : stream.invalid) {
      invalid += range.clusters;
    }
    // What no run, hole or invalid range covers is lost: a cut run list, or a
    // part of the stream that is missing.
    const ULONGLONG covered = stream.real_clusters + sparse + invalid;
    const ULONGLONG missing = stream.span > covered ? stream.span - covered : 0;
    result.tally.Add(ClusterCondition::Sparse, sparse);
    result.tally.Add(ClusterCondition::Invalid, invalid + missing);
    if (!NeedsPieces(stream)) {
      return;
    }

    std::vector<ClusterPiece> pieces = std::move(PiecesOf(account));
    for (const VcnRange& range : stream.sparse) {
      pieces.push_back({.first_vcn = range.vcn,
                        .clusters = range.clusters,
                        .lcn = std::nullopt,
                        .condition = ClusterCondition::Sparse});
    }
    for (const VcnRange& range : stream.invalid) {
      pieces.push_back({.first_vcn = range.vcn,
                        .clusters = range.clusters,
                        .lcn = std::nullopt,
                        .condition = ClusterCondition::Invalid});
    }
    std::ranges::stable_sort(pieces, {}, &ClusterPiece::first_vcn);
    pieces = FillGaps(pieces, stream.span);
    if (stream.unit_clusters != 0) {
      pieces =
          MarkBrokenUnits(pieces, BrokenUnits(pieces, stream.unit_clusters));
      for (const ClusterPiece& piece : pieces) {
        if (piece.lcn) {
          result.tally.Add(piece.condition, piece.clusters);
        }
      }
    }
    if (level_ != ClusterAccountLevel::CountsAndMap) {
      return;
    }
    result.streams.push_back({.name = stream.name,
                              .data_size = stream.data_size,
                              .initialized_size = stream.initialized_size,
                              .pieces = Merge(pieces)});
  }

  // Adds an Invalid piece wherever pieces, sorted by VCN, leave a gap in
  // [0, span).
  static std::vector<ClusterPiece>
      FillGaps(const std::vector<ClusterPiece>& pieces, ULONGLONG span) {
    std::vector<ClusterPiece> filled;
    filled.reserve(pieces.size() + 1);
    ULONGLONG at = 0;
    for (const ClusterPiece& piece : pieces) {
      if (piece.first_vcn > at) {
        filled.push_back({.first_vcn = at,
                          .clusters = piece.first_vcn - at,
                          .lcn = std::nullopt,
                          .condition = ClusterCondition::Invalid});
      }
      filled.push_back(piece);
      at = (std::max)(at, piece.first_vcn + piece.clusters);
    }
    if (span > at) {
      filled.push_back({.first_vcn = at,
                        .clusters = span - at,
                        .lcn = std::nullopt,
                        .condition = ClusterCondition::Invalid});
    }
    return filled;
  }

  // Joins each piece to the one before it when they touch in the file, share
  // a condition, and both have an address or both have none.
  static std::vector<ClusterPiece>
      Merge(const std::vector<ClusterPiece>& pieces) {
    std::vector<ClusterPiece> merged;
    merged.reserve(pieces.size());
    for (const ClusterPiece& piece : pieces) {
      if (!merged.empty()) {
        ClusterPiece& last = merged.back();
        if (last.condition == piece.condition &&
            last.first_vcn + last.clusters == piece.first_vcn &&
            last.lcn.has_value() == piece.lcn.has_value()) {
          last.clusters += piece.clusters;
          continue;
        }
      }
      merged.push_back(piece);
    }
    return merged;
  }

  const ClaimInventory* inventory_;
  ClusterAccountLevel level_;
  std::vector<ClusterAccount> results_;
  // The pieces of each account stream, at the map level.
  std::vector<std::vector<ClusterPiece>> pieces_;
  // The pieces of the compressed streams, below it.
  std::unordered_map<std::uint32_t, std::vector<ClusterPiece>> kept_;
};

}  // namespace

template <Cache::Strategy S>
std::vector<OwnerAccount>
    AccountClusters(const NtfsVolume<S>& volume, ClaimInventory& inventory,
                    ClusterAccountLevel level, DataCheckMode check) {
  std::ranges::sort(inventory.claims, [](const Claim& left,
                                         const Claim& right) {
    return std::tie(left.lcn, left.stream, left.vcn, left.clusters_and_flags) <
           std::tie(right.lcn, right.stream, right.vcn,
                    right.clusters_and_flags);
  });
  std::vector<Resolved> resolved = Sweeper<S>(volume, inventory).Run();
  // The claims are no longer needed: give their memory back before the rest.
  std::vector<Claim>().swap(inventory.claims);

  if (check != DataCheckMode::Off) {
    const ULONGLONG read =
        ContentChecker<S>(volume, inventory, check).Run(resolved);
    Log::Info("Cluster check: {} clusters read", read);
  }

  Assembler assembler(inventory, level);
  for (const Resolved& stretch : resolved) {
    assembler.Add(stretch);
  }
  return assembler.Finish();
}

template std::vector<OwnerAccount> AccountClusters<Cache::Strategy::NoCache>(
    const NtfsVolume<Cache::Strategy::NoCache>&, ClaimInventory&,
    ClusterAccountLevel, DataCheckMode);
template std::vector<OwnerAccount> AccountClusters<Cache::Strategy::FullCache>(
    const NtfsVolume<Cache::Strategy::FullCache>&, ClaimInventory&,
    ClusterAccountLevel, DataCheckMode);

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
