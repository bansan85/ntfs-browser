#pragma once

namespace NtfsBrowser
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
}  // namespace NtfsBrowser