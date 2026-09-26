#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{
// Low 48 bits of an on-disk file reference: the MFT record number.
inline constexpr ULONGLONG kMftRecordNumberMask = 0x0000FFFFFFFFFFFFULL;
// Bit position of the 16-bit sequence number in an on-disk file reference.
inline constexpr unsigned kMftSequenceShift = 48;
}  // namespace NtfsBrowser
