#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{
// Low 48 bits of an on-disk file reference: the MFT record number.
inline constexpr ULONGLONG mft_record_number_mask = 0x0000FFFFFFFFFFFFULL;
// Bit position of the 16-bit sequence number in an on-disk file reference.
inline constexpr unsigned mft_sequence_shift = 48;

// Largest 16-bit sequence number. The next one wraps to 1, not 0.
inline constexpr WORD mft_max_sequence = 0xFFFF;

// The sequence number NTFS gives a record when it frees it: one more,
// skipping 0, since a reference carrying 0 means "do not check".
constexpr WORD NextSequence(WORD sequence) noexcept
{
  return sequence == mft_max_sequence ? 1 : static_cast<WORD>(sequence + 1);
}

// True if a file reference carrying "referencedSequence" names the record's
// current generation: unchecked (0), equal, or, for a record freed since, the
// one NTFS bumped it to on deletion. A live record MUST NOT get the last rule.
constexpr bool IsSameRecordGeneration(WORD referenced_sequence,
                                      WORD record_sequence,
                                      bool record_in_use) noexcept
{
  return referenced_sequence == 0 || referenced_sequence == record_sequence ||
         (!record_in_use &&
          record_sequence == NextSequence(referenced_sequence));
}

// True if a record named by an $ATTRIBUTE_LIST entry is really an extension
// of the listing file. The entry's sequence number MAY be 0, which claims
// nothing. The record MUST name the listing file as its base, or it is a
// record another file reused.
constexpr bool
    IsGenuineExtensionRecord(WORD entry_sequence, WORD record_sequence,
                             ULONGLONG record_base_ref,
                             ULONGLONG listing_record_number) noexcept
{
  // Sequence: unclaimed or equal. Base: the listing file.
  return (entry_sequence == 0 || entry_sequence == record_sequence) &&
         (record_base_ref & mft_record_number_mask) == listing_record_number;
}
}  // namespace NtfsBrowser
