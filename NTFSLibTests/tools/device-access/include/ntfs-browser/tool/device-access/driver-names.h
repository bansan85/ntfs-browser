#pragma once

#ifndef _WIN32

  #include <optional>
  #include <string>
  #include <string_view>

namespace NtfsDevice {

// Reports whether name holds an unpaired surrogate, so it has no UTF-8 form.
// The Linux NTFS drivers each render such a name their own way, and
// NtfsCompare has to undo that rendering to compare it.
[[nodiscard]] bool NeedsDriverMapping(std::wstring_view name);

// Reports whether the mount filesystem type fstype is a driver whose
// rendering of an unpaired surrogate was measured and is decoded exactly by
// DecodeDriverName().
[[nodiscard]] bool IsDriverMappingKnown(std::string_view fstype);

// Decodes bytes, a name as the driver of fstype shows it, to the UTF-16 code
// units NTFS stores. Returns nothing for a driver whose mapping is not known:
// NtfsCompare reports that as a tool error, never as a name difference.
[[nodiscard]] std::optional<std::wstring>
    DecodeDriverName(std::string_view fstype, std::string_view bytes);

}  // namespace NtfsDevice

#endif  // !_WIN32
