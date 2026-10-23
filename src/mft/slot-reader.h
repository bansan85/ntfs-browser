#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
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

// Hands the file records of $MFT to a FileRecord one slot after the other, but
// reads them from the disk many at a time. A window of slots is read through
// $MFT's own extents, so a fragmented $MFT, and one whose DATA is split across
// extension records, reads as it does record by record. The window is cut into
// records in memory.
//
// A slot the window does not serve is read by FileRecord itself: a record below
// Mft::Idx::User (fixed address), a slot outside Expect()'s range, and every
// slot of a window whose read failed. The outcome of ParseFileRecord() is the
// one FileRecord::ParseFileRecord() gives.
//
// Slots are meant to be asked for in increasing order. Not thread safe.
template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY SlotReader {
 public:
  // block_bytes is the size of one window, rounded down to whole records and
  // up to one cluster. volume MUST outlive the reader.
  explicit SlotReader(const NtfsVolume<S>& volume,
                      size_t block_bytes = default_slot_block_bytes);

  // Declares that slots [first, last) are about to be asked for, in increasing
  // order. No window reads past last or before first. The initial range is
  // every slot of the volume. A caller that skips slots (eg. free ones) calls
  // this once per run it wants, so the skipped slots are never read.
  void Expect(ULONGLONG first, ULONGLONG last) noexcept;

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

  // Reads the window that starts at or just before record. False when the
  // read fails: the slots of that window are then marked as failed.
  [[nodiscard]] bool LoadWindow(Io::FileRecord<S>& file_record,
                                ULONGLONG record);

  const NtfsVolume<S>* volume_;
  DWORD record_size_;
  // Records in one window.
  ULONGLONG window_slots_;
  // Records in one cluster: a window starts on a cluster boundary, so the
  // read is aligned and costs no extra partial-cluster read.
  ULONGLONG cluster_slots_;

  // Slots Expect() allows.
  ULONGLONG range_first_{0};
  ULONGLONG range_last_{0};

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
