#include "upcase/upcase.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>

namespace NtfsBrowser::UpCase {
namespace {

// One run of the built-in mapping: every step-th unit from first to last
// maps to itself plus delta.
struct UpCaseRun {
  char16_t first;
  char16_t last;
  std::uint8_t step;
  std::int32_t delta;
};

// Unicode 15.1 simple uppercase mapping of the BMP, generated from the
// Unicode Character Database. Runs with step 2 are the alternating
// upper/lower pairs of the Latin, Greek and Cyrillic blocks.
constexpr auto built_in_runs = std::to_array<UpCaseRun>({
    {.first = 0x0061, .last = 0x007A, .step = 1, .delta = -32},
    {.first = 0x00B5, .last = 0x00B5, .step = 1, .delta = 743},
    {.first = 0x00E0, .last = 0x00F6, .step = 1, .delta = -32},
    {.first = 0x00F8, .last = 0x00FE, .step = 1, .delta = -32},
    {.first = 0x00FF, .last = 0x00FF, .step = 1, .delta = 121},
    {.first = 0x0101, .last = 0x012F, .step = 2, .delta = -1},
    {.first = 0x0131, .last = 0x0131, .step = 1, .delta = -232},
    {.first = 0x0133, .last = 0x0137, .step = 2, .delta = -1},
    {.first = 0x013A, .last = 0x0148, .step = 2, .delta = -1},
    {.first = 0x014B, .last = 0x0177, .step = 2, .delta = -1},
    {.first = 0x017A, .last = 0x017E, .step = 2, .delta = -1},
    {.first = 0x017F, .last = 0x017F, .step = 1, .delta = -300},
    {.first = 0x0180, .last = 0x0180, .step = 1, .delta = 195},
    {.first = 0x0183, .last = 0x0185, .step = 2, .delta = -1},
    {.first = 0x0188, .last = 0x0188, .step = 1, .delta = -1},
    {.first = 0x018C, .last = 0x018C, .step = 1, .delta = -1},
    {.first = 0x0192, .last = 0x0192, .step = 1, .delta = -1},
    {.first = 0x0195, .last = 0x0195, .step = 1, .delta = 97},
    {.first = 0x0199, .last = 0x0199, .step = 1, .delta = -1},
    {.first = 0x019A, .last = 0x019A, .step = 1, .delta = 163},
    {.first = 0x019E, .last = 0x019E, .step = 1, .delta = 130},
    {.first = 0x01A1, .last = 0x01A5, .step = 2, .delta = -1},
    {.first = 0x01A8, .last = 0x01A8, .step = 1, .delta = -1},
    {.first = 0x01AD, .last = 0x01AD, .step = 1, .delta = -1},
    {.first = 0x01B0, .last = 0x01B0, .step = 1, .delta = -1},
    {.first = 0x01B4, .last = 0x01B6, .step = 2, .delta = -1},
    {.first = 0x01B9, .last = 0x01B9, .step = 1, .delta = -1},
    {.first = 0x01BD, .last = 0x01BD, .step = 1, .delta = -1},
    {.first = 0x01BF, .last = 0x01BF, .step = 1, .delta = 56},
    {.first = 0x01C5, .last = 0x01C5, .step = 1, .delta = -1},
    {.first = 0x01C6, .last = 0x01C6, .step = 1, .delta = -2},
    {.first = 0x01C8, .last = 0x01C8, .step = 1, .delta = -1},
    {.first = 0x01C9, .last = 0x01C9, .step = 1, .delta = -2},
    {.first = 0x01CB, .last = 0x01CB, .step = 1, .delta = -1},
    {.first = 0x01CC, .last = 0x01CC, .step = 1, .delta = -2},
    {.first = 0x01CE, .last = 0x01DC, .step = 2, .delta = -1},
    {.first = 0x01DD, .last = 0x01DD, .step = 1, .delta = -79},
    {.first = 0x01DF, .last = 0x01EF, .step = 2, .delta = -1},
    {.first = 0x01F2, .last = 0x01F2, .step = 1, .delta = -1},
    {.first = 0x01F3, .last = 0x01F3, .step = 1, .delta = -2},
    {.first = 0x01F5, .last = 0x01F5, .step = 1, .delta = -1},
    {.first = 0x01F9, .last = 0x021F, .step = 2, .delta = -1},
    {.first = 0x0223, .last = 0x0233, .step = 2, .delta = -1},
    {.first = 0x023C, .last = 0x023C, .step = 1, .delta = -1},
    {.first = 0x023F, .last = 0x0240, .step = 1, .delta = 10815},
    {.first = 0x0242, .last = 0x0242, .step = 1, .delta = -1},
    {.first = 0x0247, .last = 0x024F, .step = 2, .delta = -1},
    {.first = 0x0250, .last = 0x0250, .step = 1, .delta = 10783},
    {.first = 0x0251, .last = 0x0251, .step = 1, .delta = 10780},
    {.first = 0x0252, .last = 0x0252, .step = 1, .delta = 10782},
    {.first = 0x0253, .last = 0x0253, .step = 1, .delta = -210},
    {.first = 0x0254, .last = 0x0254, .step = 1, .delta = -206},
    {.first = 0x0256, .last = 0x0257, .step = 1, .delta = -205},
    {.first = 0x0259, .last = 0x0259, .step = 1, .delta = -202},
    {.first = 0x025B, .last = 0x025B, .step = 1, .delta = -203},
    {.first = 0x025C, .last = 0x025C, .step = 1, .delta = 42319},
    {.first = 0x0260, .last = 0x0260, .step = 1, .delta = -205},
    {.first = 0x0261, .last = 0x0261, .step = 1, .delta = 42315},
    {.first = 0x0263, .last = 0x0263, .step = 1, .delta = -207},
    {.first = 0x0265, .last = 0x0265, .step = 1, .delta = 42280},
    {.first = 0x0266, .last = 0x0266, .step = 1, .delta = 42308},
    {.first = 0x0268, .last = 0x0268, .step = 1, .delta = -209},
    {.first = 0x0269, .last = 0x0269, .step = 1, .delta = -211},
    {.first = 0x026A, .last = 0x026A, .step = 1, .delta = 42308},
    {.first = 0x026B, .last = 0x026B, .step = 1, .delta = 10743},
    {.first = 0x026C, .last = 0x026C, .step = 1, .delta = 42305},
    {.first = 0x026F, .last = 0x026F, .step = 1, .delta = -211},
    {.first = 0x0271, .last = 0x0271, .step = 1, .delta = 10749},
    {.first = 0x0272, .last = 0x0272, .step = 1, .delta = -213},
    {.first = 0x0275, .last = 0x0275, .step = 1, .delta = -214},
    {.first = 0x027D, .last = 0x027D, .step = 1, .delta = 10727},
    {.first = 0x0280, .last = 0x0280, .step = 1, .delta = -218},
    {.first = 0x0282, .last = 0x0282, .step = 1, .delta = 42307},
    {.first = 0x0283, .last = 0x0283, .step = 1, .delta = -218},
    {.first = 0x0287, .last = 0x0287, .step = 1, .delta = 42282},
    {.first = 0x0288, .last = 0x0288, .step = 1, .delta = -218},
    {.first = 0x0289, .last = 0x0289, .step = 1, .delta = -69},
    {.first = 0x028A, .last = 0x028B, .step = 1, .delta = -217},
    {.first = 0x028C, .last = 0x028C, .step = 1, .delta = -71},
    {.first = 0x0292, .last = 0x0292, .step = 1, .delta = -219},
    {.first = 0x029D, .last = 0x029D, .step = 1, .delta = 42261},
    {.first = 0x029E, .last = 0x029E, .step = 1, .delta = 42258},
    {.first = 0x0345, .last = 0x0345, .step = 1, .delta = 84},
    {.first = 0x0371, .last = 0x0373, .step = 2, .delta = -1},
    {.first = 0x0377, .last = 0x0377, .step = 1, .delta = -1},
    {.first = 0x037B, .last = 0x037D, .step = 1, .delta = 130},
    {.first = 0x03AC, .last = 0x03AC, .step = 1, .delta = -38},
    {.first = 0x03AD, .last = 0x03AF, .step = 1, .delta = -37},
    {.first = 0x03B1, .last = 0x03C1, .step = 1, .delta = -32},
    {.first = 0x03C2, .last = 0x03C2, .step = 1, .delta = -31},
    {.first = 0x03C3, .last = 0x03CB, .step = 1, .delta = -32},
    {.first = 0x03CC, .last = 0x03CC, .step = 1, .delta = -64},
    {.first = 0x03CD, .last = 0x03CE, .step = 1, .delta = -63},
    {.first = 0x03D0, .last = 0x03D0, .step = 1, .delta = -62},
    {.first = 0x03D1, .last = 0x03D1, .step = 1, .delta = -57},
    {.first = 0x03D5, .last = 0x03D5, .step = 1, .delta = -47},
    {.first = 0x03D6, .last = 0x03D6, .step = 1, .delta = -54},
    {.first = 0x03D7, .last = 0x03D7, .step = 1, .delta = -8},
    {.first = 0x03D9, .last = 0x03EF, .step = 2, .delta = -1},
    {.first = 0x03F0, .last = 0x03F0, .step = 1, .delta = -86},
    {.first = 0x03F1, .last = 0x03F1, .step = 1, .delta = -80},
    {.first = 0x03F2, .last = 0x03F2, .step = 1, .delta = 7},
    {.first = 0x03F3, .last = 0x03F3, .step = 1, .delta = -116},
    {.first = 0x03F5, .last = 0x03F5, .step = 1, .delta = -96},
    {.first = 0x03F8, .last = 0x03F8, .step = 1, .delta = -1},
    {.first = 0x03FB, .last = 0x03FB, .step = 1, .delta = -1},
    {.first = 0x0430, .last = 0x044F, .step = 1, .delta = -32},
    {.first = 0x0450, .last = 0x045F, .step = 1, .delta = -80},
    {.first = 0x0461, .last = 0x0481, .step = 2, .delta = -1},
    {.first = 0x048B, .last = 0x04BF, .step = 2, .delta = -1},
    {.first = 0x04C2, .last = 0x04CE, .step = 2, .delta = -1},
    {.first = 0x04CF, .last = 0x04CF, .step = 1, .delta = -15},
    {.first = 0x04D1, .last = 0x052F, .step = 2, .delta = -1},
    {.first = 0x0561, .last = 0x0586, .step = 1, .delta = -48},
    {.first = 0x10D0, .last = 0x10FA, .step = 1, .delta = 3008},
    {.first = 0x10FD, .last = 0x10FF, .step = 1, .delta = 3008},
    {.first = 0x13F8, .last = 0x13FD, .step = 1, .delta = -8},
    {.first = 0x1C80, .last = 0x1C80, .step = 1, .delta = -6254},
    {.first = 0x1C81, .last = 0x1C81, .step = 1, .delta = -6253},
    {.first = 0x1C82, .last = 0x1C82, .step = 1, .delta = -6244},
    {.first = 0x1C83, .last = 0x1C84, .step = 1, .delta = -6242},
    {.first = 0x1C85, .last = 0x1C85, .step = 1, .delta = -6243},
    {.first = 0x1C86, .last = 0x1C86, .step = 1, .delta = -6236},
    {.first = 0x1C87, .last = 0x1C87, .step = 1, .delta = -6181},
    {.first = 0x1C88, .last = 0x1C88, .step = 1, .delta = 35266},
    {.first = 0x1D79, .last = 0x1D79, .step = 1, .delta = 35332},
    {.first = 0x1D7D, .last = 0x1D7D, .step = 1, .delta = 3814},
    {.first = 0x1D8E, .last = 0x1D8E, .step = 1, .delta = 35384},
    {.first = 0x1E01, .last = 0x1E95, .step = 2, .delta = -1},
    {.first = 0x1E9B, .last = 0x1E9B, .step = 1, .delta = -59},
    {.first = 0x1EA1, .last = 0x1EFF, .step = 2, .delta = -1},
    {.first = 0x1F00, .last = 0x1F07, .step = 1, .delta = 8},
    {.first = 0x1F10, .last = 0x1F15, .step = 1, .delta = 8},
    {.first = 0x1F20, .last = 0x1F27, .step = 1, .delta = 8},
    {.first = 0x1F30, .last = 0x1F37, .step = 1, .delta = 8},
    {.first = 0x1F40, .last = 0x1F45, .step = 1, .delta = 8},
    {.first = 0x1F51, .last = 0x1F57, .step = 2, .delta = 8},
    {.first = 0x1F60, .last = 0x1F67, .step = 1, .delta = 8},
    {.first = 0x1F70, .last = 0x1F71, .step = 1, .delta = 74},
    {.first = 0x1F72, .last = 0x1F75, .step = 1, .delta = 86},
    {.first = 0x1F76, .last = 0x1F77, .step = 1, .delta = 100},
    {.first = 0x1F78, .last = 0x1F79, .step = 1, .delta = 128},
    {.first = 0x1F7A, .last = 0x1F7B, .step = 1, .delta = 112},
    {.first = 0x1F7C, .last = 0x1F7D, .step = 1, .delta = 126},
    {.first = 0x1F80, .last = 0x1F87, .step = 1, .delta = 8},
    {.first = 0x1F90, .last = 0x1F97, .step = 1, .delta = 8},
    {.first = 0x1FA0, .last = 0x1FA7, .step = 1, .delta = 8},
    {.first = 0x1FB0, .last = 0x1FB1, .step = 1, .delta = 8},
    {.first = 0x1FB3, .last = 0x1FB3, .step = 1, .delta = 9},
    {.first = 0x1FBE, .last = 0x1FBE, .step = 1, .delta = -7205},
    {.first = 0x1FC3, .last = 0x1FC3, .step = 1, .delta = 9},
    {.first = 0x1FD0, .last = 0x1FD1, .step = 1, .delta = 8},
    {.first = 0x1FE0, .last = 0x1FE1, .step = 1, .delta = 8},
    {.first = 0x1FE5, .last = 0x1FE5, .step = 1, .delta = 7},
    {.first = 0x1FF3, .last = 0x1FF3, .step = 1, .delta = 9},
    {.first = 0x214E, .last = 0x214E, .step = 1, .delta = -28},
    {.first = 0x2170, .last = 0x217F, .step = 1, .delta = -16},
    {.first = 0x2184, .last = 0x2184, .step = 1, .delta = -1},
    {.first = 0x24D0, .last = 0x24E9, .step = 1, .delta = -26},
    {.first = 0x2C30, .last = 0x2C5F, .step = 1, .delta = -48},
    {.first = 0x2C61, .last = 0x2C61, .step = 1, .delta = -1},
    {.first = 0x2C65, .last = 0x2C65, .step = 1, .delta = -10795},
    {.first = 0x2C66, .last = 0x2C66, .step = 1, .delta = -10792},
    {.first = 0x2C68, .last = 0x2C6C, .step = 2, .delta = -1},
    {.first = 0x2C73, .last = 0x2C73, .step = 1, .delta = -1},
    {.first = 0x2C76, .last = 0x2C76, .step = 1, .delta = -1},
    {.first = 0x2C81, .last = 0x2CE3, .step = 2, .delta = -1},
    {.first = 0x2CEC, .last = 0x2CEE, .step = 2, .delta = -1},
    {.first = 0x2CF3, .last = 0x2CF3, .step = 1, .delta = -1},
    {.first = 0x2D00, .last = 0x2D25, .step = 1, .delta = -7264},
    {.first = 0x2D27, .last = 0x2D27, .step = 1, .delta = -7264},
    {.first = 0x2D2D, .last = 0x2D2D, .step = 1, .delta = -7264},
    {.first = 0xA641, .last = 0xA66D, .step = 2, .delta = -1},
    {.first = 0xA681, .last = 0xA69B, .step = 2, .delta = -1},
    {.first = 0xA723, .last = 0xA72F, .step = 2, .delta = -1},
    {.first = 0xA733, .last = 0xA76F, .step = 2, .delta = -1},
    {.first = 0xA77A, .last = 0xA77C, .step = 2, .delta = -1},
    {.first = 0xA77F, .last = 0xA787, .step = 2, .delta = -1},
    {.first = 0xA78C, .last = 0xA78C, .step = 1, .delta = -1},
    {.first = 0xA791, .last = 0xA793, .step = 2, .delta = -1},
    {.first = 0xA794, .last = 0xA794, .step = 1, .delta = 48},
    {.first = 0xA797, .last = 0xA7A9, .step = 2, .delta = -1},
    {.first = 0xA7B5, .last = 0xA7C3, .step = 2, .delta = -1},
    {.first = 0xA7C8, .last = 0xA7CA, .step = 2, .delta = -1},
    {.first = 0xA7D1, .last = 0xA7D1, .step = 1, .delta = -1},
    {.first = 0xA7D7, .last = 0xA7D9, .step = 2, .delta = -1},
    {.first = 0xA7F6, .last = 0xA7F6, .step = 1, .delta = -1},
    {.first = 0xAB53, .last = 0xAB53, .step = 1, .delta = -928},
    {.first = 0xAB70, .last = 0xABBF, .step = 1, .delta = -38864},
    {.first = 0xFF41, .last = 0xFF5A, .step = 1, .delta = -32},
});

// A valid $UpCase maps a-z to A-Z, on every volume. FromBytes() relies on
// this to reject a wiped or forged table.
constexpr char16_t lower_a = u'a';
constexpr char16_t lower_z = u'z';
constexpr char16_t case_distance = 0x20;

// Bits in one byte: the shift that joins the two bytes of a stored entry.
constexpr unsigned bits_per_byte = 8;

// First and last unit of the UTF-16 surrogate area.
constexpr char32_t high_surrogate_first = 0xD800;
constexpr char32_t low_surrogate_first = 0xDC00;
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

// Expands built_in_runs into a full table.
std::vector<char16_t> MakeBuiltInMap() {
  std::vector<char16_t> map(Table::unit_count);
  for (size_t unit = 0; unit < map.size(); unit++) {
    // unit < map.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    map[unit] = gsl::narrow<char16_t>(unit);
  }
  for (const UpCaseRun& run : built_in_runs) {
    for (std::uint32_t unit = run.first; unit <= run.last; unit += run.step) {
      map.at(unit) =
          gsl::narrow<char16_t>(gsl::narrow<std::int32_t>(unit) + run.delta);
    }
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

}  // namespace NtfsBrowser::UpCase
