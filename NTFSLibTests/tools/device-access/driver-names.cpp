#ifndef _WIN32

  #include "driver-names.h"

  #include <algorithm>
  #include <optional>
  #include <string>
  #include <string_view>

  #include "text-convert.h"

namespace NtfsDevice {

namespace {

// Mount filesystem type of ntfs-3g. Measured 2026-10-19 (WSL Ubuntu 26.04,
// ntfs-3g 2022.10.3, research R12): it shows an unpaired surrogate in its
// 3-byte WTF-8 form, which Utf8ToWide() decodes exactly. Every FUSE
// filesystem reports this type, so the caller MUST also know the mount's
// source is the NTFS volume it read.
constexpr std::string_view ntfs3g_fstype = "fuse";

// Mount filesystem type of ntfs-3g when it mounts a block device itself.
constexpr std::string_view ntfs3g_block_fstype = "fuseblk";

// First and last UTF-16 surrogate code unit.
constexpr wchar_t surrogate_first = 0xD800;
constexpr wchar_t surrogate_last = 0xDFFF;

}  // namespace

bool NeedsDriverMapping(std::wstring_view name) {
  // wchar_t is UTF-32 here, so a pair is already one value above U+FFFF and
  // any value in the surrogate range is unpaired.
  return std::ranges::any_of(name, [](wchar_t unit) {
    return unit >= surrogate_first && unit <= surrogate_last;
  });
}

bool IsDriverMappingKnown(std::string_view fstype) {
  return fstype == ntfs3g_fstype || fstype == ntfs3g_block_fstype;
}

std::optional<std::wstring> DecodeDriverName(std::string_view fstype,
                                             std::string_view bytes) {
  if (!IsDriverMappingKnown(fstype)) {
    return std::nullopt;
  }
  return Utf8ToWide(bytes);
}

}  // namespace NtfsDevice

#endif  // !_WIN32
