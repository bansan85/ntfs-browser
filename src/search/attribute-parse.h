#pragma once

// Decoders of the raw bytes of two attributes, for the Reader. They read a
// copy of the content of the attribute, never a view into the record, and they
// do not trust its sizes.

#include <cstdint>
#include <optional>
#include <span>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader-types.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// Decodes the content of a $STANDARD_INFORMATION. Nothing when it is shorter
// than the 48 bytes of the NTFS 1 form. The fields of the NTFS 3 form are set
// when the content holds all 72 bytes.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<StandardInformation>
    ParseStandardInformation(std::span<const std::uint8_t> content);

// Decodes the content of a $FILE_NAME. Nothing when it is shorter than its
// fixed part and its name, or when its name is empty. index is the position the
// result takes in FileName::index.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<FileName>
    ParseFileName(std::span<const std::uint8_t> content, std::uint16_t index);

}  // namespace NTFS_BROWSER_NAMESPACE::Search
