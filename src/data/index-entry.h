#pragma once

#include <windows.h>

#include "../flag/index-entry.h"

// OK

namespace NtfsBrowser::Data
{

struct IndexEntry
{
  // Low 6B: MFT record index, High 2B: MFT record sequence number
  ULONGLONG file_reference;
  WORD size;               // Length of the index entry
  WORD stream_size;        // Length of the stream
  Flag::IndexEntry flags;  // Flags
  BYTE padding[3];         // Padding
  BYTE stream;             // Stream
  // VCN of the sub node in Index Allocation, Offset = Size - 8
};

}  // namespace NtfsBrowser::Data