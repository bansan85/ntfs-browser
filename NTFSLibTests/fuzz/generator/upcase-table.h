#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The $UpCase table of the hand-made volumes: 65536 words, one per UTF-16
// unit, each holding the upper case form of its unit.
class UpcaseTable {
 public:
  // Units in the table, and bytes in the file.
  static constexpr std::size_t unit_count = 0x10000;
  static constexpr std::size_t file_bytes = unit_count * sizeof(WORD);

  // Every unit maps to itself, but the letters a to z.
  [[nodiscard]] static std::vector<BYTE> LettersFolded();

  // Makes table map unit to upper.
  static void Map(std::vector<BYTE>& table, WORD unit, WORD upper);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
