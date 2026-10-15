#pragma once

#include <cstdint>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Mft {

enum class Idx : std::uint8_t {
  // MFT Indexes
  Mft = 0,
  MftMirr = 1,
  LogFile = 2,
  Volume = 3,
  AttrDef = 4,
  Root = 5,
  Bitmap = 6,
  Boot = 7,
  BadCluster = 8,
  Secure = 9,
  UpCase = 10,
  Extend = 11,
  Reserved12 = 12,
  Reserved13 = 13,
  Reserved14 = 14,
  Reserved15 = 15,
  User = 16
};

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
