#pragma once

namespace NtfsBrowser
{

enum class AttrVolInfoFlag
{
  DIRTY = 0x0001,  // Dirty
  RLF = 0x0002,    // Resize logfile
  UOM = 0x0004,    // Upgrade on mount
  MONT = 0x0008,   // Mounted on NT4
  DUSN = 0x0010,   // Delete USN underway
  ROI = 0x0020,    // Repair object Ids
  MBC = 0x8000     // Modified by chkdsk
};

}  // namespace NtfsBrowser