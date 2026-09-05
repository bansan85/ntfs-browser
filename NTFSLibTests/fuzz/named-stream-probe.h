#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsFuzz
{

// UTF-16 name of the named $DATA attribute (Alternate Data Stream) used to
// exercise FindStream()'s named-stream lookup path (F18,
// docs/bug-reports/2026-09-03-full-repo.md): written into the fixture built
// by NTFSLibTests/unit-tests/fake-ntfs-image.h's
// BuildFakeNtfsImageWithNamedDataStream(), and passed to FindStream() by
// afl-main.cpp's FuzzOnce(). Shared here instead of duplicated as a string
// literal in both places, so a rename in one spot can't silently desync from
// the other. Lives in this portable (no <windows.h>) fuzz/ header, rather
// than fake-ntfs-image.h itself, so NtfsFuzzerAfl - which must keep building
// on Linux - doesn't have to include that Windows-only header to reach it.
inline constexpr wchar_t kNamedDataStreamName[] = L"F2-probe";

// kNamedDataStreamName's length in UTF-16 code units (what
// AttrHeaderCommon::name_length actually counts), independent of
// sizeof(kNamedDataStreamName) so it stays correct if the name above ever
// changes.
inline constexpr BYTE kNamedDataStreamNameLength = 8;

}  // namespace NtfsFuzz
