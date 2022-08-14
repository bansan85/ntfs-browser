#pragma once

#include <windows.h>

#include "../flag/index-entry.h"

namespace NtfsBrowser::Data
{

struct IndexEntry
{
  struct
  {
    // Low 6B : MFT record index
    ULONGLONG mft_index : 48;
    // High 2B: MFT record sequence number
    ULONGLONG mft_sn : 16;
  };
  WORD size;               // Length of the index entry
  WORD stream_size;        // Length of the stream
  Flag::IndexEntry flags;  // Flags
  BYTE padding[3];         // Padding
  BYTE stream;             // Stream
  // VCN of the sub node in Index Allocation, Offset = Size - 8
};

}  // namespace NtfsBrowser::Data