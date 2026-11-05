#pragma once

#include <cstddef>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// Test hook: when on, every refill of the standing lane works its zone out
// from the start as well, and compares it with the zone it kept from the last
// refill. A difference counts in LaneZoneMismatchesForTests() and throws. Off
// by default.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetVerifyLaneZoneForTests(bool verify) noexcept;

// Test hook: how many differences the verification found, since the process
// started.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::size_t
    LaneZoneMismatchesForTests() noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
