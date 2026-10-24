#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "../internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// Bytes of $MFT read at once by default. 4 MiB turns a scan of millions of
// 1 KiB records into a few thousand reads, and costs no more memory than one
// buffer of that size.
inline constexpr size_t default_slot_block_bytes = size_t{4} * 1024 * 1024;

// The slots [first, last) of $MFT.
struct SlotRange {
  ULONGLONG first{0};
  ULONGLONG last{0};
};

// Hands the file records of $MFT to a FileRecord one slot after the other, but
// reads them from the disk many at a time. A window of slots is read through
// $MFT's own extents, so a fragmented $MFT, and one whose DATA is split across
// extension records, reads as it does record by record. The window is cut into
// records in memory.
//
// The plan says which slots the caller will ask for. Each of its ranges is cut
// into windows from its first slot's cluster on, whatever slots the caller
// asks for, so the windows are known in advance. The next ones are announced
// to the volume's reader while the caller parses the current one, so the disk
// can read them meanwhile.
//
// A slot the window does not serve is read by FileRecord itself: a record below
// Mft::Idx::User (fixed address), a slot outside the plan, and every slot of a
// window whose read failed. The outcome of ParseFileRecord() is the one
// FileRecord::ParseFileRecord() gives.
//
// Slots are meant to be asked for in increasing order. Not thread safe.
template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY SlotReader {
 public:
  // block_bytes is the size of one window, rounded down to whole clusters, and
  // up to one cluster. volume MUST outlive the reader.
  explicit SlotReader(const NtfsVolume<S>& volume,
                      size_t block_bytes = default_slot_block_bytes);
  SlotReader(const SlotReader&) = delete;
  SlotReader& operator=(const SlotReader&) = delete;
  SlotReader(SlotReader&&) = delete;
  SlotReader& operator=(SlotReader&&) = delete;
  // Withdraws the windows it announced and did not read.
  ~SlotReader();

  // Declares the ranges of slots about to be asked for, in increasing order
  // and apart from each other. No window reads outside them: a caller that
  // skips slots (eg. free ones) leaves them out, so they are never read.
  // Replaces the previous plan, and drops the window read for it. The initial
  // plan is every slot of the volume.
  void Plan(std::vector<SlotRange> ranges);

  // Plan() with the one range [first, last).
  void Expect(ULONGLONG first, ULONGLONG last);

  // Parses slot record of $MFT into file_record, like
  // file_record.ParseFileRecord(record) does.
  [[nodiscard]] bool ParseFileRecord(Io::FileRecord<S>& file_record,
                                     ULONGLONG record);

  // The raw bytes of slot record, as the window holds them, when the last
  // ParseFileRecord() of record took it from there. Else an empty span. The
  // span is valid until the next window is read.
  [[nodiscard]] std::span<const BYTE> HeldSlot(ULONGLONG record) const;

 private:
  // The bytes of slot record, or an empty span when the window does not serve
  // it. Reads the window that holds it when none does yet.
  [[nodiscard]] std::span<const BYTE> Slot(Io::FileRecord<S>& file_record,
                                           ULONGLONG record);

  // The index in plan_ of the range that holds record, or plan_.size().
  [[nodiscard]] size_t RangeOf(ULONGLONG record) const;

  // The window of range that holds record. record MUST lie in range, at or
  // past Mft::Idx::User.
  [[nodiscard]] SlotRange WindowOf(const SlotRange& range,
                                   ULONGLONG record) const;

  // Reads window, the one range_index holds record in. False when the read
  // fails: the slots of that window are then marked as failed.
  [[nodiscard]] bool LoadWindow(Io::FileRecord<S>& file_record,
                                size_t range_index, const SlotRange& window);

  // Announces window unless it was, then the windows after it, up to the
  // prefetch caps.
  void AnnounceFrom(size_t range_index, const SlotRange& window);

  // The window after the last one announced, in plan order, or an empty one at
  // the end of the plan.
  [[nodiscard]] SlotRange NextWindow();

  const NtfsVolume<S>* volume_;
  DWORD record_size_;
  // Records in one cluster: a window starts on a cluster boundary, so the
  // read is aligned and costs no extra partial-cluster read.
  ULONGLONG cluster_slots_;
  // Records in one window: whole clusters.
  ULONGLONG window_slots_;

  std::vector<SlotRange> plan_;

  // The windows announced and not read yet, in plan order, and their slots.
  std::deque<SlotRange> announced_;
  ULONGLONG announced_slots_{0};
  // Where the next window to announce starts: a range of plan_, and a slot.
  size_t next_range_{0};
  ULONGLONG next_slot_{0};
  // Set once a window is announced: the volume then has hints to withdraw.
  bool announced_any_{false};

  // The slots [window_first_, window_last_) held in window_. Empty when equal.
  ULONGLONG window_first_{0};
  ULONGLONG window_last_{0};
  std::vector<BYTE> window_;

  // The slots of the last window whose read failed. Empty when equal.
  ULONGLONG failed_first_{0};
  ULONGLONG failed_last_{0};
};

}  // namespace Mft

}  // namespace NTFS_BROWSER_NAMESPACE
