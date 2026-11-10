#include "upcase/upcase.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>
#include <utf8proc.h>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::UpCase {
namespace {

// A valid $UpCase maps a-z to A-Z, on every volume. FromBytes() relies on
// this to reject a wiped or forged table.
constexpr char16_t lower_a = u'a';
constexpr char16_t lower_z = u'z';
constexpr char16_t case_distance = 0x20;

// Bits in one byte: the shift that joins the two bytes of a stored entry.
constexpr unsigned bits_per_byte = 8;

// First high surrogate, first low surrogate, and the unit after the last low
// surrogate. The three bound the area 0xD800-0xDFFF.
constexpr char32_t high_surrogate_first = 0xD800;
constexpr char32_t low_surrogate_first = 0xDC00;
constexpr char32_t surrogate_end = 0xE000;
// Sharp s. UnicodeData.txt gives it no simple uppercase, and neither does the
// $UpCase of Windows. utf8proc maps it to U+1E9E on purpose, so the table
// overrides that one entry.
constexpr char16_t sharp_s = 0x00DF;
// Highest code point Unicode defines.
constexpr char32_t max_code_point = 0x10FFFF;
// Stands in for a wchar_t that is not a code point.
constexpr char16_t replacement_character = 0xFFFD;
// First code point above the BMP, and the shift and mask that split the
// rest into the two 10-bit halves of a surrogate pair.
constexpr char32_t supplementary_base = 0x10000;
constexpr unsigned surrogate_shift = 10;
constexpr char32_t surrogate_mask = 0x3FF;

// Yields the UTF-16 code units of a wide string one at a time. On Windows
// wchar_t already is a UTF-16 unit. Elsewhere it holds a whole code point,
// which becomes a surrogate pair.
class Utf16Cursor {
 public:
  explicit Utf16Cursor(std::wstring_view text) noexcept : text_(text) {}

  [[nodiscard]] bool AtEnd() const noexcept {
    return pending_low_ == 0 && index_ >= text_.size();
  }

  [[nodiscard]] char16_t Next() noexcept {
    if (pending_low_ != 0) {
      const char16_t low = pending_low_;
      pending_low_ = 0;
      return low;
    }

    // Callers test AtEnd() first, so index_ < text_.size().
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto value = static_cast<char32_t>(text_[index_++]);
    if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
      return static_cast<char16_t>(value);
    } else {
      if (value < supplementary_base) {
        return static_cast<char16_t>(value);
      }
      if (value > max_code_point) {
        return replacement_character;
      }
      const char32_t offset = value - supplementary_base;
      pending_low_ = static_cast<char16_t>(low_surrogate_first +
                                           (offset & surrogate_mask));
      return static_cast<char16_t>(high_surrogate_first +
                                   (offset >> surrogate_shift));
    }
  }

 private:
  std::wstring_view text_;
  size_t index_{0};
  char16_t pending_low_{0};
};

// Expands the Unicode simple uppercase mapping of utf8proc into a full table.
// A surrogate maps to itself: NTFS folds UTF-16 units, never code points.
std::vector<char16_t> MakeBuiltInMap() {
  std::vector<char16_t> map(Table::unit_count);
  for (size_t index = 0; index < map.size(); index++) {
    const auto unit = gsl::narrow<char16_t>(index);
    const bool keeps_unit =
        (unit >= high_surrogate_first && unit < surrogate_end) ||
        unit == sharp_s;
    const auto upper =
        keeps_unit ? static_cast<utf8proc_int32_t>(unit)
                   : utf8proc_toupper(static_cast<utf8proc_int32_t>(unit));
    // No BMP unit has a supplementary uppercase. A table entry holds one
    // unit, so such a value would stay unmapped.
    // index < map.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    map[index] = upper <= std::numeric_limits<char16_t>::max()
                     ? gsl::narrow<char16_t>(upper)
                     : unit;
  }
  return map;
}

}  // namespace

// Private: callers go through BuiltIn() or FromBytes().
Table::Table(std::vector<char16_t> map, bool built_in)
    : map_(std::move(map)), built_in_(built_in) {}

const Table& Table::BuiltIn() {
  static const Table table(MakeBuiltInMap(), true);
  return table;
}

std::optional<Table> Table::FromBytes(std::span<const BYTE> bytes) {
  if (bytes.size() < Table::byte_count) {
    return std::nullopt;
  }

  std::vector<char16_t> map(Table::unit_count);
  for (size_t unit = 0; unit < map.size(); unit++) {
    const size_t offset = unit * sizeof(char16_t);
    // bytes.size() >= Table::byte_count = 2 * map.size(), so offset + 1
    // is in range. unit < map.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto low = bytes[offset];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto high = bytes[offset + 1];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    map[unit] =
        static_cast<char16_t>(static_cast<unsigned>(low) |
                              (static_cast<unsigned>(high) << bits_per_byte));
  }

  for (char16_t unit = lower_a; unit <= lower_z; unit++) {
    // unit is an ASCII letter, well below the 65536 entries of map.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (map[unit] != static_cast<char16_t>(unit - case_distance)) {
      return std::nullopt;
    }
  }

  return Table(std::move(map), false);
}

bool Table::IsBuiltIn() const noexcept { return built_in_; }

char16_t Table::Map(char16_t unit) const noexcept {
  // Both factories build Table::unit_count entries, one per char16_t
  // value.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return map_[unit];
}

int Table::Compare(std::wstring_view first,
                   std::wstring_view second) const noexcept {
  Utf16Cursor left(first);
  Utf16Cursor right(second);
  while (!left.AtEnd() && !right.AtEnd()) {
    const char16_t left_unit = Map(left.Next());
    const char16_t right_unit = Map(right.Next());
    if (left_unit != right_unit) {
      return left_unit < right_unit ? -1 : 1;
    }
  }
  if (left.AtEnd() && right.AtEnd()) {
    return 0;
  }
  return left.AtEnd() ? -1 : 1;
}

}  // namespace NTFS_BROWSER_NAMESPACE::UpCase
