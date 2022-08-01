#pragma once

#include <windows.h>

namespace NtfsBrowser::Data
{

struct IndexEntry
{
  ULONGLONG
  FileReference;  // Low 6B: MFT record index, High 2B: MFT record sequence number
  WORD Size;      // Length of the index entry
  WORD StreamSize;  // Length of the stream
  BYTE Flags;       // Flags
  BYTE Padding[3];  // Padding
  BYTE Stream[1];   // Stream
  // VCN of the sub node in Index Allocation, Offset = Size - 8
};

}  // namespace NtfsBrowser::Data