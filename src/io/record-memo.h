#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// What a probe of a file record reads of it, and no more: the signature, the
// sequence number, the flags, the update sequence array and the last word of
// each sector. A recursive NoCache listing meets the neighbours of the records
// it probed again, long after the window of $MFT dropped them. This keeps those
// few bytes for many more records than the window holds, in a table of a fixed
// size: a record takes the place its slot number gives, and a later one with
// the same place replaces it. The table comes from the system zeroed and
// unused places are never written, so its memory is committed as it fills. Not
// thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY RecordMemo {
 public:
  // entries is the size of the table: 0 holds nothing. record_size is the size
  // of a file record. A record size that is not a multiple of 512 bytes, or
  // that is over 4096, holds nothing either.
  RecordMemo(std::size_t entries, std::size_t record_size);

  // Whether the memo holds anything at all.
  [[nodiscard]] bool Enabled() const noexcept { return entries_ != 0; }

  // Bytes of the table: 32 for each entry of 1 KiB records.
  [[nodiscard]] std::uint64_t TableBytes() const noexcept;

  // Keeps what a probe reads of the records that bytes spans, read from $MFT
  // from slot first_slot on. A record whose header a probe would judge by other
  // bytes (not a FILE record, an update sequence array out of the first sector)
  // is not kept, and drops an older record of the same place. A partial record
  // at the end is ignored.
  void Put(ULONGLONG first_slot, std::span<const BYTE> bytes);

  // A record that a probe judges as it judges the record Put() saw: the kept
  // bytes, zeros elsewhere. Empty when the memo does not hold it. The bytes
  // stay valid until the next Find().
  [[nodiscard]] std::span<const BYTE> Find(ULONGLONG slot);

 private:
  // Frees a table that std::calloc gave.
  struct FreeTable {
    void operator()(BYTE* table) const noexcept { std::free(table); }
  };

  // Keeps the record at the start of bytes, which is slot slot, or drops what
  // its place held.
  void PutOne(ULONGLONG slot, std::span<const BYTE> bytes);

  // The bytes of the place that slot takes.
  [[nodiscard]] std::span<BYTE> PlaceOf(ULONGLONG slot) noexcept;

  std::size_t record_size_;
  // Sectors of a record, each ending with an update sequence word.
  std::size_t sectors_{0};
  // Places in the table, and the bytes of one: the slot number plus one (0 is
  // an empty place), then the offset and size of the array, the sequence number
  // and the flags, the array, and the last two bytes of each sector.
  std::size_t entries_{0};
  std::size_t stride_{0};
  std::unique_ptr<BYTE[], FreeTable> table_;
  // The record Find() rebuilds.
  std::vector<BYTE> rebuilt_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
