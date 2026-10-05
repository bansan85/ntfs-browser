#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsFuzz
{

// Named-stream name shared between fake-ntfs-image.h's fixture and
// afl-main.cpp's FuzzOnce(), so the two can't silently drift apart.
inline constexpr std::wstring_view named_data_stream_name = L"ads-name";

// named_data_stream_name's length in UTF-16 code units.
inline constexpr BYTE named_data_stream_name_length = 8;

}  // namespace NtfsFuzz
