#pragma once

#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "cache/strategy.h"
#include "mft/slot-reader.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// Bits in one byte of the $MFT bitmap.
inline constexpr ULONGLONG bits_per_byte = 8;

// Whether bitmap marks record as free. The bitmap covers every record the
// scan visits, so record / 8 is in range.
inline bool IsMarkedFree(const std::vector<BYTE>& bitmap,
                         ULONGLONG record) noexcept {
  // ReadMftBitmap() sized bitmap to cover all slots, and record is one of them.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const BYTE bits = bitmap[record / bits_per_byte];
  return (bits & (1U << (record % bits_per_byte))) == 0;
}

// Reads the unnamed $BITMAP of $MFT: one bit per record slot, set when the
// slot is in use. Returns nullopt, and the scan then reads every slot, when
// record 0 or its bitmap cannot be read or the bitmap does not cover total
// slots.
template <Cache::Strategy S>
std::optional<std::vector<BYTE>> ReadMftBitmap(const NtfsVolume<S>& volume,
                                               ULONGLONG total);

// Slots bitmap marks in use among the total it covers.
ULONGLONG CountUsedSlots(const std::vector<BYTE>& bitmap,
                         ULONGLONG total) noexcept;

// The ranges of slots the scan reads with a bitmap: each starts at a used slot
// and ends where the last used slot before a gap of more than max_gap_slots
// free ones lies, or at total. The free slots inside are read with it, but not
// parsed.
std::vector<Mft::SlotRange> PlanSlotRanges(const std::vector<BYTE>& bitmap,
                                           ULONGLONG total,
                                           ULONGLONG max_gap_slots);

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
