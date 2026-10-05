#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsFuzz
{

// Sorts smaller than the sub-node root entry's name only under NTFS' real
// uppercase collation, not lowercase folding.
inline constexpr std::wstring_view gap_collation_search_name = L"AB";

// gap_collation_search_name's length in UTF-16 code units.
inline constexpr BYTE gap_collation_search_name_length = 2;

}  // namespace NtfsFuzz
