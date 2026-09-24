#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{
// Low 48 bits of an on-disk file reference: the MFT record number.
inline constexpr ULONGLONG kMftRecordNumberMask = 0x0000FFFFFFFFFFFFULL;
// Bit position of the 16-bit sequence number in an on-disk file reference.
inline constexpr unsigned kMftSequenceShift = 48;
}  // namespace NtfsBrowser

namespace NtfsBrowser::Enum
{

enum class MftIdx
{
  // MFT Indexes
  MFT = 0,
  MFT_MIRR = 1,
  LOG_FILE = 2,
  VOLUME = 3,
  ATTR_DEF = 4,
  ROOT = 5,
  BITMAP = 6,
  BOOT = 7,
  BAD_CLUSTER = 8,
  SECURE = 9,
  UPCASE = 10,
  EXTEND = 11,
  RESERVED12 = 12,
  RESERVED13 = 13,
  RESERVED14 = 14,
  RESERVED15 = 15,
  USER = 16
};
}  // namespace NtfsBrowser::Enum