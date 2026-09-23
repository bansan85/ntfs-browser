#pragma once

// LZNT1 decompression, the compression format classic NTFS attribute-level
// compression (FILE_ATTRIBUTE_COMPRESSED plus a non-zero
// Attr::HeaderNonResident::comp_unit_size) stores its compression units in.
//
// Written from scratch against the public [MS-XCA] specification
// (docs/[MS-XCA].pdf), section 2.5 "LZNT1 Algorithm Details" - Microsoft's
// Open Specifications documentation, whose IP notice explicitly permits
// copying portions of it into an implementation, keeping this library's
// licensing Apache/MIT-compatible. No GPL/LGPL LZNT1 implementation
// (ms-compress, libfwnt, ...) was consulted.
//
// Decompression only: this library is read-only, so there is deliberately
// no encoder here (nor anywhere else, including the test fixtures - those
// build their compressed input from [MS-XCA] section 3.3's own worked
// example bytes and from trivially hand-encoded "uncompressed chunk"
// headers).
//
// Internal implementation detail of AttrNonResident<S>::ReadData(): nothing
// under include/ntfs-browser/ declares, forward-declares or re-exports
// anything from this header.
//
// Portable C++ only (no Win32/POSIX APIs), so it builds in both supported
// configurations: MSVC/Windows and GCC/Linux.

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>

#include "../internal-export.h"

namespace NtfsBrowser::Lznt1
{

// LZNT1 chunk size; [MS-XCA] 2.5.3 fixes streams at 4096-byte units.
inline constexpr size_t kChunkSize = 4096;

// Decompresses one LZNT1 buffer, "src", into "dest" and returns the byte
// count written. "src" MUST hold exactly the compressed data; "dest" MUST
// fit the output. Throws std::runtime_error on malformed input.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t
    Decompress(std::span<const BYTE> src, std::span<BYTE> dest);

}  // namespace NtfsBrowser::Lznt1
