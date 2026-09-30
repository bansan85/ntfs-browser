#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace NtfsBrowser
{
namespace Flag
{
enum class IndexEntry : BYTE;
}  // namespace Flag
}  // namespace NtfsBrowser

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

// An index entry seen through a properly aligned pointer. The bytes it points
// at stay alive as long as `owner` does.
struct AlignedIndexEntry
{
  std::shared_ptr<BYTE[]> owner;
  const Data::IndexEntry* entry;
};

// Copies out the fixed part of the entry at `at`, whatever its alignment.
// The caller MUST have checked that offsetof(Data::IndexEntry, stream) bytes
// fit at `at`.
[[nodiscard]] Data::IndexEntry ReadIndexEntryHeader(const BYTE* at) noexcept;

// Returns the entry of `size` bytes at `at`, a position inside `buffer`. An
// entry that is not aligned for Data::IndexEntry moves to an aligned copy
// first. The caller MUST have checked that `size` bytes fit at `at`.
[[nodiscard]] AlignedIndexEntry
    AlignIndexEntry(const std::shared_ptr<BYTE[]>& buffer, const BYTE* at,
                    size_t size);

// Checks ie's on-disk bounds and sub-node size. Returns the defect message
// if one is found, or none if ie is well-formed. Callers log it through
// LogRecoverable, at whichever level (strict vs. recovering) applies there.
[[nodiscard]] std::optional<std::string_view>
    ValidateIndexEntry(const Data::IndexEntry& ie) noexcept;

}  // namespace NtfsBrowser