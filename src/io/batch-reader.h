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

#include "cache/strategy.h"
#include "io/block-batch-reader.h"
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
  // The directories whose blocks may be read ahead.
  using Candidate = typename BlockBatchReader<S>::Candidate;

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

  // The records of wanted that the budget of the walk has room for: cuts wanted
  // to them. False when none fits. A record costs its size.
  [[nodiscard]] bool FitToBudget(std::vector<ULONGLONG>& wanted) const;

  // The records of upcoming, in the window of the listing, that are not held
  // and may be read ahead, each once. held is the count of those already held.
  [[nodiscard]] std::vector<ULONGLONG>
      WantedOf(std::span<const ULONGLONG> upcoming, std::size_t& held) const;

  const NtfsVolume<S>* volume_;
  HeldStore* store_;
  RecordKeeper* keeper_;
  ProbeLane<S>* lane_;
  ReadAheadCapture* capture_;
  DirectoryReadAheadStats* stats_;
  Tuning::WalkTuning walk_;
  Tuning::ProbeTuning probe_;
  BlockBatchReader<S> blocks_;
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
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
