#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "mft/held-records.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// The extension records the second pass reads for one waiting base record, that
// the first pass could not hold. It reads them before it parses the record, in
// increasing order and run by run: that is the order the reader was told, so it
// serves them from what it read ahead. A record the list names several times
// is read once. Holds at most MftTuning::max_preread_extensions records.
template <Cache::Strategy S>
class UnheldExtensions {
 public:
  explicit UnheldExtensions(const NtfsVolume<S>& volume);

  // Reads the records of the first max_preread_ of records, in
  // increasing order. A run that cannot be read is left out: the record is
  // then read from the disk when asked for, as when it is not here.
  void Load(const std::vector<ULONGLONG>& records);

  // The bytes of record, or an empty span when it was not loaded.
  [[nodiscard]] std::span<const BYTE> Find(ULONGLONG record) const;

  // Forgets every record.
  void Clear() noexcept;

 private:
  // Reads the run records from first on, and remembers them.
  void Read(ULONGLONG first, size_t run);

  const NtfsVolume<S>* volume_;
  size_t record_size_;
  // Records read at once at most, and records in one run at most.
  size_t max_preread_;
  size_t max_run_;
  std::vector<BYTE> bytes_;
  std::unordered_map<ULONGLONG, size_t> offsets_;
};

// Announces to the volume's reader what the second pass reads from the disk, a
// number of waiting records ahead of it and in the order it reads it: the
// record when it was not held, its extension records that were not held, and
// its $ATTRIBUTE_LIST stream. The disk reads them while the pass parses.
//
// The reader serves a read only from the first announced ranges, and drops
// those before the one it serves. A range no read uses, because the pass does
// not read what was announced, would block every later one. Restart() clears
// them.
template <Cache::Strategy S>
class ReadAnnouncer {
 public:
  ReadAnnouncer(const NtfsVolume<S>& volume,
                const std::vector<HeldRecords::Waiting>& waiting);

  // Called before the pass parses waiting[index]. Announces the records that
  // follow those already announced, until max_announced_records of them, or
  // max_announced_ranges ranges, wait from index on (MftTuning).
  void Before(size_t index);

  // Whether the pass has passed the restart interval's records with reads
  // since the last restart.
  [[nodiscard]] bool RestartDue() const noexcept;

  // Withdraws everything announced and starts again at waiting[index]: the
  // pass did not read what it announced for the record before.
  void Restart(size_t index);

 private:
  // Whether the pass reads anything for waiting from the disk.
  [[nodiscard]] static bool HasReads(const HeldRecords::Waiting& waiting);

  // Announces the reads of waiting, in the order the pass makes them, and
  // returns how many ranges it announced.
  size_t Announce(const HeldRecords::Waiting& waiting) const;

  const NtfsVolume<S>* volume_;
  const std::vector<HeldRecords::Waiting>* waiting_;
  const Tuning::MftTuning* tuning_;
  // The ranges announced for each record, 0 when none or not announced yet.
  std::vector<size_t> ranges_;
  // Records the pass parsed, and those among them that had reads announced,
  // since the last restart.
  size_t passed_{0};
  size_t passed_with_reads_{0};
  // The first record whose reads are not announced.
  size_t next_{0};
  // Records with reads, and their ranges, announced among those from passed_
  // to next_.
  size_t ahead_records_{0};
  size_t ahead_ranges_{0};
  bool cancelled_{false};
};

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
