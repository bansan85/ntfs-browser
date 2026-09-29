#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{
// Low 48 bits of an on-disk file reference: the MFT record number.
inline constexpr ULONGLONG kMftRecordNumberMask = 0x0000FFFFFFFFFFFFULL;
// Bit position of the 16-bit sequence number in an on-disk file reference.
inline constexpr unsigned kMftSequenceShift = 48;

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
