#include "io/block-batch-reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/index-alloc.h"
#include "attr/index-block.h"
#include "cache/strategy.h"
#include "index-entry.h"
#include "io/directory-read-ahead.h"
#include "io/read-ahead-held.h"
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

}  // namespace

template <Cache::Strategy S>
BlockBatchReader<S>::BlockBatchReader(const NtfsVolume<S>& volume,
                                      ReadAheadShared& shared)
    : volume_(&volume),
      store_(&shared.store),
      capture_(&shared.capture),
      walk_(volume.GetOptions().tuning.walk),
      probe_(volume.GetOptions().tuning.probe),
      cache_block_bytes_(CacheBlockBytes(volume.GetOptions().tuning.cache)) {}

template <Cache::Strategy S>
void BlockBatchReader<S>::AnnounceExtent(const Alloc::Extent& extent) {
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
void BlockBatchReader<S>::ReadBlocks(std::span<const Candidate> candidates) {
  std::vector<Candidate> level(candidates.begin(), candidates.end());
  for (std::size_t depth = 0; !level.empty() && depth < max_read_levels;
       depth++) {
    const std::vector<Want> read = ReadLevel(level, depth != 0);
    level = ChildrenOf(read);
  }
}

template <Cache::Strategy S>
std::vector<typename BlockBatchReader<S>::Want>
    BlockBatchReader<S>::WantsOf(const Candidate& candidate,
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
void BlockBatchReader<S>::FitToShare(std::vector<Want>& own,
                                     ULONGLONG directory, std::uint64_t planned,
                                     DWORD block_size) const {
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
std::vector<typename BlockBatchReader<S>::Want>
    BlockBatchReader<S>::ReadLevel(std::span<const Candidate> level,
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
std::vector<typename BlockBatchReader<S>::Want>
    BlockBatchReader<S>::HoldWants(std::span<Want> wants, DWORD block_size) {
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
void BlockBatchReader<S>::AddSubNodes(const Want& want,
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
std::vector<typename BlockBatchReader<S>::Candidate>
    BlockBatchReader<S>::ChildrenOf(std::span<const Want> read) {
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

template class BlockBatchReader<Cache::Strategy::NoCache>;
template class BlockBatchReader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
