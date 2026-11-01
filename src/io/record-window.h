#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// The runs of $MFT that a recursive listing read last, held so that the next
// probe of a neighbouring record does not read them again. A NoCache volume
// keeps nothing, and the probes of two directories that sit side by side in
// $MFT read the same stretch twice: the hole that a run reads across, and
// the records of two directories that were created together. Bounded by a
// byte budget, the oldest run goes first. Not thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY RecordWindow {
 public:
  // budget is the most bytes held: 0 holds nothing. record_size is the size of
  // a file record in bytes, at least 1.
  RecordWindow(std::uint64_t budget, std::size_t record_size);

  // Whether the window holds anything at all.
  [[nodiscard]] bool Enabled() const noexcept { return budget_ != 0; }

  // Bytes held now.
  [[nodiscard]] std::uint64_t HeldBytes() const noexcept { return held_; }

  // The bytes of record slot, empty when the window does not hold it. They
  // stay valid until the next Put().
  [[nodiscard]] std::span<const BYTE> Find(ULONGLONG slot) const;

  // Holds the records that bytes spans, read from $MFT from slot first_slot
  // on, and drops the oldest runs until the budget fits. A run that alone
  // outgrows the budget is not held. A partial record at the end is ignored.
  void Put(ULONGLONG first_slot, std::span<const BYTE> bytes);

 private:
  // One run read from $MFT.
  struct Run {
    ULONGLONG first_slot;
    std::vector<BYTE> bytes;
  };

  // Where the newest copy of a record sits.
  struct Place {
    // Serial number of the run that holds it.
    std::uint64_t serial;
    // Index of the record in that run.
    std::size_t index;
  };

  // Drops the oldest run, and the places that still point to it.
  void DropOldest();

  std::uint64_t budget_;
  std::size_t record_size_;
  std::uint64_t held_{0};
  // Serial number of runs_.front(), which the next run follows.
  std::uint64_t first_serial_{0};
  std::deque<Run> runs_;
  std::unordered_map<ULONGLONG, Place> places_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
