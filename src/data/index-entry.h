#pragma once

#include <optional>
#include <string_view>

#include <ntfs-browser/win-types.h>

#include "../flag/index-entry.h"

namespace NtfsBrowser::Data
{

struct IndexEntry
{
  // Low 6B : MFT record index
  ULONGLONG mft_index : 48;
  // High 2B: MFT record sequence number
  ULONGLONG mft_sn : 16;
  WORD size;               // Length of the index entry
  WORD stream_size;        // Length of the stream
  Flag::IndexEntry flags;  // Flags
  BYTE padding[3];         // Padding
  BYTE stream;             // Stream
  // VCN of the sub node in Index Allocation, Offset = Size - 8
};

}  // namespace NtfsBrowser::Data

namespace NtfsBrowser
{

// Checks ie's on-disk bounds and sub-node size. Returns the defect message
// if one is found, or none if ie is well-formed. Callers log it through
// LogRecoverable, at whichever level (strict vs. recovering) applies there.
[[nodiscard]] std::optional<std::string_view>
    ValidateIndexEntry(const Data::IndexEntry& ie) noexcept;

}  // namespace NtfsBrowser