#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsFuzz
{

// Sorts smaller than the sub-node root entry's name only under NTFS' real
// uppercase collation, not lowercase folding.
inline constexpr wchar_t kGapCollationSearchName[] = L"AB";

// kGapCollationSearchName's length in UTF-16 code units.
inline constexpr BYTE kGapCollationSearchNameLength = 2;

}  // namespace NtfsFuzz
