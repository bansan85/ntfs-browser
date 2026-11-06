#pragma once

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Mft {

// Bytes of one $ATTRIBUTE_LIST announced at most: the largest Windows writes.
// A forged size announces no more.
inline constexpr ULONGLONG max_announced_list_bytes = ULONGLONG{256} * 1024;

// Slots of $MFT whose base record number the first pass remembers at most. The
// numbers take 4 bytes each: 2^26 slots, 64 GiB of $MFT at 1 KiB per record,
// are 256 MiB. A slot past it keeps its disk read.
inline constexpr ULONGLONG max_remembered_slots = ULONGLONG{1} << 26;

// Slots the first pass reserves room for at most, before it knows how many it
// meets: 2^24 are 64 MiB, and cover 16 GiB of $MFT.
inline constexpr ULONGLONG max_reserved_slots = ULONGLONG{1} << 24;

// What SlotBases keeps for a slot the first pass did not read, or whose base is
// too large for 4 bytes.
inline constexpr std::uint32_t unknown_base =
    (std::numeric_limits<std::uint32_t>::max)();

// The base record each slot of $MFT named when the first pass read it: 0 for a
// base record, the base's record number for an extension. A record an
// $ATTRIBUTE_LIST names, that is not held, and whose slot names another base
// than the list's own, is not an extension of it: the second pass skips it
// without reading it again.
class SlotBases {
 public:
  explicit SlotBases(ULONGLONG total) {
    bases_.reserve(gsl::narrow<size_t>((std::min)(total, max_reserved_slots)));
  }

  // Remembers that slot record names base. Slots below Mft::Idx::User are
  // not remembered: the disk reads them, with the $MFTMirr fallback.
  void Note(ULONGLONG record, ULONGLONG base) {
    if (record < static_cast<ULONGLONG>(Mft::Idx::User) ||
        record >= max_remembered_slots) {
      return;
    }
    const auto slot = gsl::narrow<size_t>(record);
    if (bases_.size() <= slot) {
      bases_.resize(slot + 1, unknown_base);
    }
    // resize() above covers slot.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bases_[slot] =
        base < unknown_base ? static_cast<std::uint32_t>(base) : unknown_base;
  }

  // The base record number slot record named, or nothing when it is unknown.
  [[nodiscard]] std::optional<ULONGLONG> BaseOf(ULONGLONG record) const {
    if (record >= bases_.size()) {
      return std::nullopt;
    }
    // record < size() was just checked.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const std::uint32_t base = bases_[gsl::narrow<size_t>(record)];
    if (base == unknown_base) {
      return std::nullopt;
    }
    return base;
  }

 private:
  std::vector<std::uint32_t> bases_;
};

// Raw $MFT records a scan holds for its second pass, as the slot reader read
// them: the extension records that the waiting base records will ask for, and
// those base records. Both kinds share one budget, MaxHeldRecordBytes(). A
// record that does not fit is not held: a base record waits as its number, an
// extension record is listed with its base, and the second pass announces both
// to the reader.
//
// An extension record is of use to its base record only. A base met before it
// is known to wait or not. A base met after it is not yet: the extension is
// held provisionally, and released once the base turns out not to wait.
class HeldRecords {
 public:
  // A base record that waits for the second pass, with the entry kept in its
  // place, and what its first header read counted.
  struct Waiting {
    ULONGLONG record;
    size_t entry_index;
    bool header_damaged;
    // Where its bytes are held. Nothing when the budget was spent: the second
    // pass reads the record again.
    std::optional<size_t> slot;
    // The clusters its $ATTRIBUTE_LIST is read from, if it is not resident.
    std::vector<Alloc::Extent> list_extents;
    // Its extension records no slot holds, in increasing order: the second
    // pass reads them from the disk.
    std::vector<ULONGLONG> unheld_extensions;
  };

  // block_bytes is the size of one block of held records, and budget the bytes
  // held at most.
  HeldRecords(size_t record_size, size_t block_bytes, size_t budget)
      : record_size_(record_size),
        block_slots_(record_size != 0
                         ? (std::max)(size_t{1}, block_bytes / record_size)
                         : 1),
        budget_(budget) {}

  // Meets extension record, whose base is base_record, with the bytes the slot
  // reader read. Records MUST come in increasing order.
  void MeetExtension(ULONGLONG record, ULONGLONG base_record,
                     std::span<const BYTE> bytes) {
    if (base_record < record) {
      if (Waiting* const waiting = FindWaiting(base_record)) {
        Keep({.record = record, .slot = {}}, bytes, *waiting);
      }
      return;
    }
    // An extension that names itself has no base to ask for it.
    if (base_record == record) {
      return;
    }

    Provisional added{.record = record, .slot = {}};
    size_t slot = 0;
    if (Store(bytes, slot)) {
      added.slot = slot;
    }
    pending_[base_record].push_back(added);
  }

  // Holds base record for the second pass, with the extension records met
  // before it that it will ask for. False when bytes is not a record, which
  // the first pass then parses at once. When the budget is spent, the record
  // waits without its bytes.
  bool HoldWaiting(ULONGLONG record, std::span<const BYTE> bytes,
                   size_t entry_index, bool header_damaged,
                   std::vector<Alloc::Extent> list_extents) {
    if (bytes.size() != record_size_) {
      return false;
    }
    Waiting added{.record = record,
                  .entry_index = entry_index,
                  .header_damaged = header_damaged,
                  .slot = {},
                  .list_extents = std::move(list_extents),
                  .unheld_extensions = {}};
    size_t slot = 0;
    if (Store(bytes, slot)) {
      added.slot = slot;
    }
    for (const Provisional& extension : TakePending(record)) {
      Keep(extension, {}, added);
    }
    waiting_.push_back(std::move(added));
    return true;
  }

  // Keeps the held extension records met before base_record, which the first
  // pass parses at once: it asks for them in the middle of the scan.
  void Confirm(ULONGLONG base_record) {
    for (const Provisional& extension : TakePending(base_record)) {
      if (extension.slot) {
        extensions_.emplace(extension.record, *extension.slot);
      }
    }
  }

  // Releases the extension records met before a base record that precedes
  // record and did not wait: nothing asks for them.
  void ReleaseBefore(ULONGLONG record) {
    while (!pending_.empty() && pending_.begin()->first < record) {
      Release(pending_.begin()->second);
      pending_.erase(pending_.begin());
    }
  }

  // Ends the first pass: no base record is left to come.
  void Finish() {
    for (const auto& [base, extensions] : pending_) {
      Release(extensions);
    }
    pending_.clear();
  }

  // The bytes of extension record, or an empty span when it is not held.
  [[nodiscard]] std::span<const BYTE> FindExtension(ULONGLONG record) const {
    const auto found = extensions_.find(record);
    if (found == extensions_.end()) {
      return {};
    }
    return Bytes(found->second);
  }

  [[nodiscard]] const std::vector<Waiting>& WaitingRecords() const noexcept {
    return waiting_;
  }

  // The bytes held in slot, which Store() gave.
  [[nodiscard]] std::span<const BYTE> Bytes(size_t slot) const {
    const std::vector<BYTE>& block = blocks_.at(slot / block_slots_);
    return std::span<const BYTE>{block}.subspan(
        (slot % block_slots_) * record_size_, record_size_);
  }

 private:
  // An extension record met before its base record: held, or not for lack of
  // room.
  struct Provisional {
    ULONGLONG record;
    std::optional<size_t> slot;
  };

  // The waiting base record record, or null.
  [[nodiscard]] Waiting* FindWaiting(ULONGLONG record) {
    const auto found =
        std::ranges::lower_bound(waiting_, record, {}, &Waiting::record);
    return found != waiting_.end() && found->record == record ? &*found
                                                              : nullptr;
  }

  // Gives waiting the extension record it asks for: from the slot it was held
  // in, or from the disk if it was not. bytes is that record's, when it is not
  // held yet.
  void Keep(const Provisional& extension, std::span<const BYTE> bytes,
            Waiting& waiting) {
    std::optional<size_t> slot = extension.slot;
    if (!slot) {
      size_t stored = 0;
      if (Store(bytes, stored)) {
        slot = stored;
      }
    }
    if (slot) {
      extensions_.emplace(extension.record, *slot);
    } else {
      waiting.unheld_extensions.push_back(extension.record);
    }
  }

  // Removes and returns what waits for base_record.
  std::vector<Provisional> TakePending(ULONGLONG base_record) {
    const auto found = pending_.find(base_record);
    if (found == pending_.end()) {
      return {};
    }
    std::vector<Provisional> taken = std::move(found->second);
    pending_.erase(found);
    return taken;
  }

  // Frees the slots of extensions for the next records to reuse.
  void Release(const std::vector<Provisional>& extensions) {
    for (const Provisional& extension : extensions) {
      if (extension.slot) {
        free_slots_.push_back(*extension.slot);
        live_--;
      }
    }
  }

  // Copies bytes, one record, into a free slot, or the next one. False when
  // bytes is not a record or the budget is spent.
  bool Store(std::span<const BYTE> bytes, size_t& slot) {
    if (bytes.size() != record_size_) {
      return false;
    }
    if (!free_slots_.empty()) {
      slot = free_slots_.back();
      free_slots_.pop_back();
      std::vector<BYTE>& block = blocks_.at(slot / block_slots_);
      const size_t offset = (slot % block_slots_) * record_size_;
      std::ranges::copy(
          bytes, std::next(block.begin(), gsl::narrow<std::ptrdiff_t>(offset)));
      live_++;
      return true;
    }
    if ((live_ + 1) * record_size_ > budget_) {
      return false;
    }
    if (allocated_ % block_slots_ == 0) {
      blocks_.emplace_back().reserve(block_slots_ * record_size_);
    }
    std::vector<BYTE>& block = blocks_.back();
    block.insert(block.end(), bytes.begin(), bytes.end());
    slot = allocated_++;
    live_++;
    return true;
  }

  size_t record_size_;
  // Records in one block.
  size_t block_slots_;
  // Bytes this scan may hold.
  size_t budget_;
  // Slots given out so far, free ones included, and slots in use among them.
  size_t allocated_{0};
  size_t live_{0};
  // Slots Release() gave back.
  std::vector<size_t> free_slots_;
  std::vector<std::vector<BYTE>> blocks_;
  // The held extension records, by record, and their slot.
  std::unordered_map<ULONGLONG, size_t> extensions_;
  // The extension records met before their base record, by that base.
  std::map<ULONGLONG, std::vector<Provisional>> pending_;
  // Sorted by record: the scan meets them in that order.
  std::vector<Waiting> waiting_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
