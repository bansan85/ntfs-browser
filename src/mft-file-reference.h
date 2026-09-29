#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{
// Low 48 bits of an on-disk file reference: the MFT record number.
inline constexpr ULONGLONG kMftRecordNumberMask = 0x0000FFFFFFFFFFFFULL;
// Bit position of the 16-bit sequence number in an on-disk file reference.
inline constexpr unsigned kMftSequenceShift = 48;

// The sequence number NTFS gives a record when it frees it: one more,
// skipping 0, since a reference carrying 0 means "do not check".
constexpr WORD NextSequence(WORD sequence) noexcept
{
  return sequence == 0xFFFF ? 1 : static_cast<WORD>(sequence + 1);
}

// True if a file reference carrying "referencedSequence" names the record's
// current generation: unchecked (0), equal, or, for a record freed since, the
// one NTFS bumped it to on deletion. A live record MUST NOT get the last rule.
constexpr bool IsSameRecordGeneration(WORD referencedSequence,
                                      WORD recordSequence,
                                      bool recordInUse) noexcept
{
  return referencedSequence == 0 || referencedSequence == recordSequence ||
         (!recordInUse && recordSequence == NextSequence(referencedSequence));
}

// True if a record named by an $ATTRIBUTE_LIST entry is really an extension
// of the listing file. The entry's sequence number MAY be 0, which claims
// nothing. The record MUST name the listing file as its base, or it is a
// record another file reused.
constexpr bool IsGenuineExtensionRecord(WORD entrySequence, WORD recordSequence,
                                        ULONGLONG recordBaseRef,
                                        ULONGLONG listingRecordNumber) noexcept
{
  // Sequence: unclaimed or equal. Base: the listing file.
  return (entrySequence == 0 || entrySequence == recordSequence) &&
         (recordBaseRef & kMftRecordNumberMask) == listingRecordNumber;
}
}  // namespace NtfsBrowser
