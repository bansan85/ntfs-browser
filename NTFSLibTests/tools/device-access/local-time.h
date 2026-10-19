#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>

namespace NtfsDevice {

// A calendar date and time of day, in the host's local zone.
struct LocalTime {
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
};

// Converts a UTC tick count (100 ns since 1601-01-01, a FILETIME's unit) to
// the host's local calendar fields. Returns nothing for a time the platform
// cannot represent.
[[nodiscard]] std::optional<LocalTime> UtcTicksToLocalTime(ULONGLONG ticks);

}  // namespace NtfsDevice
