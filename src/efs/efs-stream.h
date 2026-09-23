#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <vector>

#include "../internal-export.h"

namespace NtfsBrowser::Efs
{

// One user's copy of the FEK, as the $EFS stream stores it.
struct WrappedFek
{
  std::vector<BYTE> thumbprint;   // SHA-1 hash of the user's certificate
  std::vector<BYTE> wrapped_fek;  // the FEK, RSA-encrypted, little-endian
};

// Parses the $EFS stream: the data decryption field (DDF), then the data
// recovery field (DRF) entries. Copies what it keeps, so the result outlives
// the stream. Returns nullopt if the stream is malformed: a bounds error, a
// count or length out of range, or a thumbprint that is not a SHA-1 hash.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY
    std::optional<std::vector<WrappedFek>>
    ParseEfsStream(std::span<const BYTE> stream);

}  // namespace NtfsBrowser::Efs
