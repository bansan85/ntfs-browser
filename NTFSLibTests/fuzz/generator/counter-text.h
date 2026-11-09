#pragma once

#include <span>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The text that the NPS 2009 generator wrote into logfile1.txt: the lines
// "This is line N.", numbered from 0 and ended by CR LF. The text has no end:
// a caller asks for the bytes it needs.
class CounterText {
 public:
  // Writes into out the bytes of the text that start at offset.
  static void Fill(ULONGLONG offset, std::span<BYTE> out);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
