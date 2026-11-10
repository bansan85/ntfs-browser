#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <ntfs-browser/namespace.h>

#include "cache/strategy.h"
#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Io {

template <Cache::Strategy S>
class FileRecord;

// What a DirectoryReadAhead has read and held so far.
struct DirectoryReadAheadStats {
  // Bytes held now, and the most it ever held. They never exceed
  // Tuning::WalkTuning::read_ahead_bytes.
  std::uint64_t held_bytes{0};
  std::uint64_t peak_held_bytes{0};
  // Records and index blocks read ahead, and how many of them the listing
  // took.
  std::uint64_t records_read{0};
  std::uint64_t records_served{0};
  std::uint64_t blocks_read{0};
  std::uint64_t blocks_served{0};
  // Records and blocks released before the listing took them.
  std::uint64_t records_unused{0};
  std::uint64_t blocks_unused{0};
  // Records that the window of Tuning::WalkTuning::record_window_bytes gave
  // in place of a read of $MFT, to the probes, the batches and the listing.
  std::uint64_t window_records{0};
  // Records that the memo of Tuning::WalkTuning::record_memo_entries gave to a
  // probe, which had read none of the record's bytes but its header.
  std::uint64_t memo_records{0};
  // Batches made: each reads the records of the planned directories, then
  // their blocks.
  std::uint64_t batches{0};
  // Probe runs of $MFT that the held directories will make, as planned: the
  // standing lane's queue. Runs announced on the lane, and runs withdrawn from
  // it before the listing read them. Runs the listing read, as planned. Runs
  // it read that no plan named. Runs not announced because the record window
  // already held every record of them.
  std::uint64_t runs_planned{0};
  std::uint64_t runs_announced{0};
  std::uint64_t runs_withdrawn{0};
  std::uint64_t runs_read{0};
  std::uint64_t runs_unplanned{0};
  std::uint64_t runs_window_held{0};
};

// Reads ahead the directories that a recursive listing will enter next, so that
// a listing of many small directories costs a few sweeps of the disk and not
// one seek for each record and each index block.
//
// A recursive listing reads, for each directory, its file record, then its
// $INDEX_ALLOCATION blocks one at a time: small reads, each waiting for the
// last (read pattern M3). A hint cannot hide them, since the walk of a
// directory withdraws what it announced between its own reads. This class
// groups them instead. For the directories it is told about, it reads their
// records together, sorted by place in $MFT, then the blocks the $INDEX_ROOT
// of each points to (the first level of the B+ tree), sorted by place on the
// disk, each batch announced to the reader first. It then parses those blocks,
// as the walk does, and reads the blocks they name in the same way, level by
// level down to the leaves: the leaves of a big directory lie side by side,
// and the walk would read them one at a time, each waiting for the last. It
// holds the bytes, and the FileRecord the listing parses with takes them from
// here instead of reading them: nothing else changes. The parse of a held
// record or block is the parse of one read from the disk, with the same lines
// logged and the same result.
//
// It reads nothing the listing would not read, apart from the $MFT slots
// between two planned records that a run reads across (the gap of
// Tuning::ProbeTuning). It logs nothing: it parses a record only to find its
// blocks, in a record of its own, and drops what that logged. A record or a
// block it could not read is not held, and the listing reads it itself. Each
// held record and block is released when Release() says the listing is done
// with its directory, used or not.
//
// A NoCache volume keeps nothing, so the probes of two directories that sit
// side by side in $MFT would read the same clusters twice: this class also
// keeps the runs of $MFT that the probes and its own batches read last, up to
// Tuning::WalkTuning::record_window_bytes, and hands them to the listing's
// FileRecord (FileRecord::SetParseOptions()). A FullCache
// volume has its block cache and keeps none. The listing, its log and the
// reads that are not held do not change. It also keeps, for many more records
// than the window holds, the few bytes of a record that a probe reads
// (Tuning::WalkTuning::record_memo_entries): the neighbours of a probed record
// come back long after the window dropped them.
//
// The probes of a directory read a few runs of $MFT, to tell which entry names
// a live, a freed or a reused record. Once the index of a held directory is
// held, this class knows those runs (PlanProbeRuns()), and announces the next
// Tuning::WalkTuning::read_ahead_runs of them on the reader's standing lane
// (IDiskReader::PrefetchStanding()), the rest of the directory being listed
// first, then the held directories of the stack from the top down. The probes
// of a directory only know their own group, and withdraw what they announced
// between two groups: the lane keeps the next directories' reads going while
// the listing waits for one. It refills as each run is read (FileRecord::
// ProbeStore::ran), withdraws the runs of a directory that Release() frees and
// every run left when it is destroyed. A run that leaves the first runs stays
// on the lane while it is among the next read_ahead_runs: the stack of a
// listing grows and shrinks, and a withdrawal of a read already made costs it
// twice. A hint: no byte more is read, and the
// listing, its log and its reads do not change. It is off at 0, and on a
// device that says it is rotational, where the index blocks wait behind the
// probes announced far ahead (Tuning::WalkTuning::read_ahead_runs_rotational).
//
// On by default (Tuning::WalkTuning::read_ahead_dirs is 64). At 0 every member
// does nothing, and the listing reads as it did before this class.
// Not thread safe, like the rest of the library.
template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY DirectoryReadAhead {
 public:
  // Reads the limits from volume.GetOptions().tuning.walk. volume MUST outlive
  // this object.
  explicit DirectoryReadAhead(const NtfsVolume<S>& volume);
  DirectoryReadAhead(const DirectoryReadAhead&) = delete;
  DirectoryReadAhead& operator=(const DirectoryReadAhead&) = delete;
  DirectoryReadAhead(DirectoryReadAhead&&) = delete;
  DirectoryReadAhead& operator=(DirectoryReadAhead&&) = delete;
  // Detaches from the record it was attached to.
  ~DirectoryReadAhead();

  // Whether the read ahead is on: it plans at least one directory.
  [[nodiscard]] bool Enabled() const noexcept;

  // Directories Plan() looks at, the top of the listing's stack: 0 when off.
  [[nodiscard]] std::size_t Window() const noexcept;

  // Makes listing, the FileRecord the listing parses its directories with,
  // take the bytes held here. Nothing when off. listing MUST outlive this
  // object, and MUST NOT be attached to another read ahead.
  void Attach(FileRecord<S>& listing);

  // Reads ahead the index blocks, every level, of the directory the attached
  // record holds now, which ParseAttrs() has parsed. For the directory a
  // listing starts at, whose record is already read. Nothing when off, or
  // when nothing is attached.
  void Begin();

  // Reads ahead the directories upcoming names, the next one first, which are
  // not held yet. They are record numbers, as the listing gives them to
  // ParseFileRecord(). A repeat, or a number below Mft::Idx::User, is left
  // out. While at least half of the first Window() of them are held, it reads
  // nothing, so that a batch reads several directories and not one at a time,
  // but for one case: when the next directory is not held, a batch of that
  // directory alone is read, since the listing enters it first. It stops short
  // of Tuning::WalkTuning::read_ahead_bytes: what does not fit waits for a
  // later call, once Release() has freed some.
  void Plan(std::span<const ULONGLONG> upcoming);

  // The listing is done with directory, entered or skipped: frees its record
  // and its blocks, and withdraws the probe runs of it that no read took. A
  // directory that is not held is ignored.
  void Release(ULONGLONG directory);

  [[nodiscard]] DirectoryReadAheadStats GetStats() const noexcept;

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
