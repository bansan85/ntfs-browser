#pragma once

#include <ntfs-browser/win-types.h>

#include <limits>
#include <optional>

#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

// Byte offset of record file_ref within $MFT's data. Empty when the offset
// does not fit a LONGLONG. The check runs before the multiplication: a
// wrapped product would alias an earlier record.
inline std::optional<ULONGLONG> RecordOffset(DWORD record_size,
                                             ULONGLONG file_ref) {
  // Largest offset a LONGLONG can hold, so every later sum stays in range.
  constexpr ULONGLONG max_offset = std::numeric_limits<LONGLONG>::max();
  if (file_ref > max_offset / record_size) {
    Log::Warn("File record {} lies past the end of $MFT", file_ref);
    return std::nullopt;
  }
  return record_size * file_ref;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
