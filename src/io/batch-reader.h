#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "cache/strategy.h"
#include "io/directory-read-ahead.h"
#include "io/mft-runs.h"
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

template <Cache::Strategy S>
class FileRecord;

template <Cache::Strategy S>
class ProbeLane;

// The batches of a DirectoryReadAhead: reads the file records of the
// directories the listing enters next, sorted by place in $MFT, then the
// blocks of their index, level by level down to the leaves, sorted by place on
// the disk, each read announced first. Holds what it read in a HeldStore. It
// logs nothing: it parses a record only to find its blocks, in a record of its
// own, and drops what that logged. Not thread safe.
template <Cache::Strategy S>
class BatchReader {
 public:
  // The batches read through volume, hold and keep into shared, and hand the
  // directories whose index sits in the record to lane. Everything MUST
  // outlive this object.
  BatchReader(const NtfsVolume<S>& volume, ReadAheadShared& shared,
              ProbeLane<S>& lane);
  BatchReader(const BatchReader&) = delete;
  BatchReader& operator=(const BatchReader&) = delete;
  BatchReader(BatchReader&&) = delete;
  BatchReader& operator=(BatchReader&&) = delete;
  ~BatchReader() = default;

  // The batch of DirectoryReadAhead::Plan(): reads ahead the records and
  // blocks of the directories of upcoming that are not held.
  void ReadUpcoming(std::span<const ULONGLONG> upcoming);

  // Reads ahead the blocks of directory, every level, from the sub-node VCNs
  // of its $INDEX_ROOT. alloc belongs to a record that outlives the batch.
  void ReadRootBlocks(ULONGLONG directory, Attr::AttrIndexAlloc<S>* alloc,
                      std::vector<ULONGLONG> vcns);

 private:
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

  // Runs body as a batch: the reads are of pattern M3, it MUST NOT log, and
  // what it counts as salvaged is counted again when the listing reaches the
  // record. A failed batch costs the gain only: the listing reads what is not
  // held.
  void Quietly(const std::function<void()>& body);

  // Reads and holds the records of wanted, then the blocks they point to.
  void ReadRecordsAhead(std::span<const ULONGLONG> wanted);

  // A record of its own, in which a held record is parsed to find its blocks.
  [[nodiscard]] std::unique_ptr<FileRecord<S>> MakeScratch();

  // Reads the records of wanted in sorted runs of $MFT, announced first, and
  // holds each. Returns those held, in the order of wanted.
  [[nodiscard]] std::vector<ULONGLONG>
      FetchRecords(std::span<const ULONGLONG> wanted);

  // Holds the records of chunk that the window has, and leaves the others in
  // missing_.
  void HoldFromWindow(std::span<const ULONGLONG> chunk);

  // Reads the runs of $MFT that cover missing_, announced first.
  void ReadMissing();

  // The runs of $MFT that cover missing_, as the probes cut them.
  [[nodiscard]] std::vector<MftRun> PlanMissingRuns() const;

  // Reads one run of missing_ from $MFT, keeps it in the window and holds each
  // record of it that was wanted.
  void ReadRun(const MftRun& run);

  // Notes that the read ahead failed to read record: the listing reads it
  // itself, and it is not tried again.
  void NoteUnreadable(ULONGLONG record);

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
  RecordKeeper* keeper_;
  ProbeLane<S>* lane_;
  ReadAheadCapture* capture_;
  DirectoryReadAheadStats* stats_;
  Tuning::WalkTuning walk_;
  Tuning::ProbeTuning probe_;
  // Bytes of one block the FullCache reads.
  std::size_t cache_block_bytes_;
  // Records that a read ahead failed to read.
  std::unordered_set<ULONGLONG> unreadable_;
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

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
