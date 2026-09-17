#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsFuzz
{

// Named-stream name shared between fake-ntfs-image.h's fixture and
// afl-main.cpp's FuzzOnce(), so the two can't silently drift apart.
inline constexpr wchar_t kNamedDataStreamName[] = L"ads-name";

// kNamedDataStreamName's length in UTF-16 code units.
inline constexpr BYTE kNamedDataStreamNameLength = 8;

}  // namespace NtfsFuzz
