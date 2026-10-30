#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <span>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

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
  // Batches made: each reads the records of the planned directories, then
  // their blocks.
  std::uint64_t batches{0};
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
// disk, each batch announced to the reader first. It holds the bytes, and the
// FileRecord the listing parses with takes them from here instead of reading
// them: nothing else changes. The parse of a held record or block is the parse
// of one read from the disk, with the same lines logged and the same result.
// Deeper levels of the tree are read by the walk, as without it.
//
// It reads nothing the listing would not read, apart from the $MFT slots
// between two planned records that a run reads across (the gap of
// Tuning::ProbeTuning). It logs nothing: it parses a record only to find its
// blocks, in a record of its own, and drops what that logged. A record or a
// block it could not read is not held, and the listing reads it itself. Each
// held record and block is released when Release() says the listing is done
// with its directory, used or not.
//
// Off, which is the default (Tuning::WalkTuning::read_ahead_dirs is 0), every
// member does nothing, and the listing reads as it always did. Not thread
// safe, like the rest of the library.
template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT DirectoryReadAhead {
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

  // Reads ahead the first level of index blocks of the directory the attached
  // record holds now, which ParseAttrs() has parsed. For the directory a
  // listing starts at, whose record is already read. Nothing when off, or
  // when nothing is attached.
  void Begin();

  // Reads ahead the directories upcoming names, the next one first, which are
  // not held yet. They are record numbers, as the listing gives them to
  // ParseFileRecord(). A repeat, or a number below Mft::Idx::User, is left
  // out. It does nothing while at least half of the first Window() of them are
  // held, so that a batch reads several directories and not one at a time. It
  // stops short of Tuning::WalkTuning::read_ahead_bytes: what does not fit
  // waits for a later call, once Release() has freed some.
  void Plan(std::span<const ULONGLONG> upcoming);

  // The listing is done with directory, entered or skipped: frees its record
  // and its blocks. A directory that is not held is ignored.
  void Release(ULONGLONG directory);

  [[nodiscard]] DirectoryReadAheadStats GetStats() const noexcept;

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
