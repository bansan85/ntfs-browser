#include "upcase-table.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The lower case letters of ASCII, and the distance to their capitals.
constexpr WORD first_letter = u'a';
constexpr WORD last_letter = u'z';
constexpr WORD case_distance = u'a' - u'A';

// The shift and the mask that split a word into its two bytes.
constexpr unsigned byte_bits = 8;
constexpr WORD byte_mask = 0xFF;

}  // namespace

std::vector<BYTE> UpcaseTable::LettersFolded() {
  std::vector<BYTE> table(file_bytes);
  for (std::size_t unit = 0; unit < unit_count; ++unit) {
    const bool letter = unit >= first_letter && unit <= last_letter;
    Map(table, static_cast<WORD>(unit),
        static_cast<WORD>(letter ? unit - case_distance : unit));
  }
  return table;
}

void UpcaseTable::Map(std::vector<BYTE>& table, WORD unit, WORD upper) {
  table.at((static_cast<std::size_t>(unit) * sizeof(WORD)) + 1) =
      static_cast<BYTE>(upper >> byte_bits);
  table.at(static_cast<std::size_t>(unit) * sizeof(WORD)) =
      static_cast<BYTE>(upper & byte_mask);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
