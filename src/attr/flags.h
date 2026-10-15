#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Attr {

// The bit of an attribute header's flags that marks its stream as compressed.
inline constexpr WORD flag_compressed = 0x0001;

// The bit of an attribute header's flags that marks its stream as encrypted.
inline constexpr WORD flag_encrypted = 0x4000;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
