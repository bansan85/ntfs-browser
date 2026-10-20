#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Usn {

// Decodes the records of a $J stream from its bytes, and finds the next record
// again after damage. It reads no disk and knows nothing of the volume: the
// caller hands it the bytes of each real range, in order, with their offset in
// the stream. Every byte is untrusted, and none can make it fail: a stretch
// that is not a record is skipped and counted.
//
// A range is a run of bytes without a hole. The caller calls Reset() before the
// first Feed() of a range, and Finish() after its last. A record may be cut by
// the end of one Feed() and finished by the next: the scanner keeps the cut
// part. Not thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY RecordScanner {
 public:
  // Starts a range: drops any bytes kept from the one before, and trusts the
  // first header it meets, as it does after any good record.
  void Reset() noexcept;

  // Decodes the records in bytes, which sit at stream_offset of $J and follow
  // what the last Feed() of this range gave. Appends them to out, with their
  // time as the UTC FILETIME the record holds. A record cut by the end of
  // bytes is finished by the next Feed().
  void Feed(std::span<const BYTE> bytes, ULONGLONG stream_offset,
            std::vector<Record>& out);

  // Ends a range. What is left is a record that no Feed() finished: it counts
  // as damage, unless the bytes are all zero, and the records that follow it in
  // the bytes kept are found again and appended to out.
  void Finish(std::vector<Record>& out);

  // Stretches that were neither a record nor padding, skipped so far.
  [[nodiscard]] ULONGLONG DamagedRecords() const noexcept;

  // Well-formed records with no name (V4), counted and not kept.
  [[nodiscard]] ULONGLONG SkippedRecords() const noexcept;

 private:
  // Decodes pending_ as far as it can, and keeps what is left. At the end of a
  // range (last), a record whose end is missing is damage like any other, and
  // nothing is kept.
  void Drain(std::vector<Record>& out, bool last);

  // Decodes the record at the start of window. Appends it to out when it has a
  // name. Returns its length in bytes, or 0 when the bytes are not a
  // well-formed record.
  [[nodiscard]] size_t DecodeRecord(std::span<const BYTE> window,
                                    ULONGLONG offset, std::vector<Record>& out);

  // Bytes that no complete record has used yet, and where the first one sits in
  // the stream.
  std::vector<BYTE> pending_;
  ULONGLONG pending_offset_{0};
  // True from a failure until a record is found again.
  bool resyncing_{false};
  ULONGLONG damaged_{0};
  ULONGLONG skipped_{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Usn
