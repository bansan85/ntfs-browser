#include "mft/used-slots.h"

#include <bit>
#include <exception>
#include <memory>
#include <optional>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "attr/base.h"
#include "attr/mask.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/idx.h"
#include "mft/slot-entry.h"
#include "mft/slot-reader.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// End (exclusive) of the slots one block read serves, from the used slot
// first: up to the last used slot before a gap of more than max_gap_slots free
// ones, or total.
ULONGLONG ReadRangeEnd(const std::vector<BYTE>& bitmap, ULONGLONG first,
                       ULONGLONG total, ULONGLONG max_gap_slots) noexcept {
  ULONGLONG last_used = first;
  for (ULONGLONG slot = first + 1;
       slot < total && slot - last_used <= max_gap_slots; slot++) {
    if (!IsMarkedFree(bitmap, slot)) {
      last_used = slot;
    }
  }
  return last_used + 1;
}

}  // namespace

template <Cache::Strategy S>
std::optional<std::vector<BYTE>> ReadMftBitmap(const NtfsVolume<S>& volume,
                                               ULONGLONG total) {
  try {
    Io::FileRecord<S> mft_record(volume);
    mft_record.SetAttrMask(Attr::Mask::Bitmap);
    if (!mft_record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Mft)) ||
        !TryParseAttrs(mft_record)) {
      return std::nullopt;
    }

    const ULONGLONG needed = (total + bits_per_byte - 1) / bits_per_byte;
    for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
         mft_record.GetAttr(Attr::Type::Bitmap)) {
      if (!attr->IsUnNamed() || attr->GetDataSize() < needed) {
        continue;
      }
      std::vector<BYTE> bits(gsl::narrow<size_t>(needed));
      const std::optional<ULONGLONG> read =
          attr->ReadData(0, {bits.data(), bits.size()});
      if (read && *read == needed) {
        return bits;
      }
    }
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
  return std::nullopt;
}

ULONGLONG CountUsedSlots(const std::vector<BYTE>& bitmap,
                         ULONGLONG total) noexcept {
  ULONGLONG used = 0;
  for (const BYTE bits : bitmap) {
    used += static_cast<ULONGLONG>(std::popcount(bits));
  }
  // The bits of the last byte past total cover no slot.
  const auto tail_bits = static_cast<unsigned>(total % bits_per_byte);
  if (tail_bits != 0 && !bitmap.empty()) {
    const auto past_total =
        static_cast<BYTE>(bitmap.back() & ~((1U << tail_bits) - 1U));
    used -= static_cast<ULONGLONG>(std::popcount(past_total));
  }
  return used;
}

std::vector<Mft::SlotRange> PlanSlotRanges(const std::vector<BYTE>& bitmap,
                                           ULONGLONG total,
                                           ULONGLONG max_gap_slots) {
  std::vector<Mft::SlotRange> plan;
  ULONGLONG record = 0;
  while (record < total) {
    if (IsMarkedFree(bitmap, record)) {
      record++;
      continue;
    }
    const ULONGLONG end = ReadRangeEnd(bitmap, record, total, max_gap_slots);
    plan.push_back({.first = record, .last = end});
    record = end;
  }
  return plan;
}

template std::optional<std::vector<BYTE>>
    ReadMftBitmap<Cache::Strategy::NoCache>(
        const NtfsVolume<Cache::Strategy::NoCache>&, ULONGLONG);
template std::optional<std::vector<BYTE>>
    ReadMftBitmap<Cache::Strategy::FullCache>(
        const NtfsVolume<Cache::Strategy::FullCache>&, ULONGLONG);

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
