#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "cache/strategy.h"
#include "io/directory-read-ahead.h"
#include "io/read-ahead-held.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Attr {

template <Cache::Strategy S>
class AttrIndexAlloc;

}  // namespace Attr

namespace Io {

// The index blocks of a BatchReader: reads the blocks of the directories it
// is given, level by level down to the leaves, sorted by place on the disk,
// each read announced first, and holds them in a HeldStore. It stops at the
// byte budget of the walk. The caller captures what it logs. Not thread safe.
template <Cache::Strategy S>
class BlockBatchReader {
 public:
  // A directory whose blocks may be read ahead: its $INDEX_ALLOCATION, and the
  // sub-node VCNs of its $INDEX_ROOT. alloc belongs to a record that outlives
  // the batch.
  struct Candidate {
    ULONGLONG directory;
    Attr::AttrIndexAlloc<S>* alloc;
    std::vector<ULONGLONG> vcns;
  };

  // The blocks are read through volume and held in shared. Both MUST outlive
  // this object.
  BlockBatchReader(const NtfsVolume<S>& volume, ReadAheadShared& shared);
  BlockBatchReader(const BlockBatchReader&) = delete;
  BlockBatchReader& operator=(const BlockBatchReader&) = delete;
  BlockBatchReader(BlockBatchReader&&) = delete;
  BlockBatchReader& operator=(BlockBatchReader&&) = delete;
  ~BlockBatchReader() = default;

  // Reads the blocks of candidates sorted by place on the disk, announced
  // first, and holds each, then the blocks those name, level by level down to
  // the leaves. Stops at the byte budget.
  void ReadBlocks(std::span<const Candidate> candidates);

 private:
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

  // Reads and holds the blocks that level names, one level of the trees. A
  // level below the first stops at a quarter of the byte budget for each
  // directory. Returns the blocks it held.
  [[nodiscard]] std::vector<Want> ReadLevel(std::span<const Candidate> level,
                                            bool below_first);

  // The blocks of candidate that are not held, are not sparse, and have a
  // place on the disk.
  [[nodiscard]] std::vector<Want> WantsOf(const Candidate& candidate,
                                          DWORD block_size) const;

  // Cuts own to what the directory and the budget have room for, given the
  // bytes planned for earlier directories of the level: the walk reads the
  // blocks that do not fit.
  void FitToShare(std::vector<Want>& own, ULONGLONG directory,
                  std::uint64_t planned, DWORD block_size) const;

  // Reads the blocks of wants, in their order, in rounds of one announcement,
  // and holds each. Returns those held.
  [[nodiscard]] std::vector<Want> HoldWants(std::span<Want> wants,
                                            DWORD block_size);

  // Announces the disk range of extent, unless an earlier announcement of the
  // round covers it.
  void AnnounceExtent(const Alloc::Extent& extent);

  // The sub-node VCNs of the held blocks in read: the next level of the trees.
  // Parses each block as the walk does, and drops what that logs.
  [[nodiscard]] std::vector<Candidate> ChildrenOf(std::span<const Want> read);

  // Adds to child the sub-node VCNs of the block want, but for those in seen.
  void AddSubNodes(const Want& want, std::unordered_set<ULONGLONG>& seen,
                   Candidate& child);

  const NtfsVolume<S>* volume_;
  HeldStore* store_;
  ReadAheadCapture* capture_;
  Tuning::WalkTuning walk_;
  Tuning::ProbeTuning probe_;
  // Bytes of one block the FullCache reads.
  std::size_t cache_block_bytes_;
  // End of the last announcement of the round, where blocks cannot overlap.
  std::optional<ULONGLONG> announced_end_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
