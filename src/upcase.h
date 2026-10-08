#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "internal-export.h"

namespace NtfsBrowser {

// Number of UTF-16 code units $UpCase maps: every unit of the BMP.
inline constexpr size_t up_case_unit_count = 65536;

// Size of $UpCase's $DATA stream: one 16-bit entry per unit, so 128 KiB.
inline constexpr size_t up_case_byte_count =
    up_case_unit_count * sizeof(char16_t);

// The uppercase mapping NTFS collates file names by. A volume stores its own
// in $UpCase (MFT record 10). The built-in one stands in when that record
// cannot be read: it is fixed data, and never depends on the process locale.
class NTFS_BROWSER_EXPORT_TESTS_ONLY UpCaseTable {
 public:
  // Unicode simple uppercase mapping of the BMP, as compiled into the library.
  [[nodiscard]] static const UpCaseTable& BuiltIn();

  // Parses the raw $UpCase stream: little-endian 16-bit entries, one per
  // code unit. Empty when the stream is short or cannot be a case mapping.
  [[nodiscard]] static std::optional<UpCaseTable>
      FromBytes(std::span<const BYTE> bytes);

  // True for BuiltIn(): the mapping may then differ from the volume's own.
  [[nodiscard]] bool IsBuiltIn() const noexcept;

  [[nodiscard]] char16_t Map(char16_t unit) const noexcept;

  // Orders a and b as NTFS does: unit by unit through the mapping, the
  // shorter name first on a common prefix. A code point above the BMP counts
  // as its surrogate pair, whatever the width of wchar_t.
  // Returns <0, 0 or >0.
  [[nodiscard]] int Compare(std::wstring_view first,
                            std::wstring_view second) const noexcept;

 private:
  UpCaseTable(std::vector<char16_t> map, bool built_in);

  std::vector<char16_t> map_;
  bool built_in_;
};

}  // namespace NtfsBrowser
