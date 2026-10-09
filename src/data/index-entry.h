#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace NtfsBrowser::Flag {

enum class IndexEntry : BYTE;

}  // namespace NtfsBrowser::Flag

namespace NtfsBrowser::Data {

struct IndexEntry {
  // Low 6B : MFT record index
  ULONGLONG mft_index : 48;
  // High 2B: MFT record sequence number
  ULONGLONG mft_sn : 16;
  WORD size;               // Length of the index entry
  WORD stream_size;        // Length of the stream
  Flag::IndexEntry flags;  // Flags
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE padding[3];  // Padding
  BYTE stream;      // Stream
  // VCN of the sub node in Index Allocation, Offset = Size - 8
};

}  // namespace NtfsBrowser::Data

namespace NtfsBrowser::Data {

// Copies out the fixed part of the entry at the start of `at`, whatever its
// alignment. The caller MUST have checked that offsetof(IndexEntry,
// stream) bytes fit in `at`.
[[nodiscard]] IndexEntry
    ReadIndexEntryHeader(std::span<const BYTE> bytes) noexcept;

// Returns the entry of `size` bytes at the start of `bytes`. An entry that is
// not aligned for IndexEntry moves to an aligned copy first, which is
// appended to `realigned`: that vector MUST outlive the returned reference.
// The caller MUST have checked that `size` bytes fit in `bytes`.
[[nodiscard]] const IndexEntry&
    AlignIndexEntry(std::vector<std::vector<BYTE>>& realigned,
                    std::span<const BYTE> bytes, size_t size);

// Checks ie's on-disk bounds and sub-node size. Returns the defect message
// if one is found, or none if ie is well-formed. Callers log it through
// Log::Recoverable, at whichever level (strict vs. recovering) applies there.
[[nodiscard]] std::optional<std::string_view>
    ValidateIndexEntry(const IndexEntry& index_entry) noexcept;

}  // namespace NtfsBrowser::Data
