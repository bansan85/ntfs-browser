#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// One line of a layers file: a kind, then words. A word with an equals sign is
// a named field ("spc=8"). A word without one is positional ("ptrn 4096
// 512"). A number is decimal, or hexadecimal after "0x".
class LayerLine {
 public:
  // Splits text into words. Throws std::runtime_error when a name repeats.
  explicit LayerLine(std::string_view text);

  // The first word of the line.
  [[nodiscard]] const std::string& Kind() const;

  // True when the line has a field called name.
  [[nodiscard]] bool Has(std::string_view name) const;

  // The value of the field called name. Throws std::runtime_error, naming the
  // line, when the field is missing.
  [[nodiscard]] const std::string& Text(std::string_view name) const;

  // The field called name as a number, or fallback when the line has no such
  // field. Throws std::runtime_error when the value is no number.
  [[nodiscard]] ULONGLONG Number(std::string_view name,
                                 ULONGLONG fallback) const;

  // The field called name as a number. Throws std::runtime_error when it is
  // missing or no number.
  [[nodiscard]] ULONGLONG Number(std::string_view name) const;

  // The field called name as the bytes of a hexadecimal text, or no byte when
  // the line has no such field.
  [[nodiscard]] std::vector<BYTE> Bytes(std::string_view name) const;

  // The positional word at index, as a number. Index 0 is the first word after
  // the kind. Throws std::runtime_error when it is missing or no number.
  [[nodiscard]] ULONGLONG Positional(std::size_t index) const;

  // The text of the line, for messages.
  [[nodiscard]] const std::string& Source() const;

 private:
  // Throws std::runtime_error with what and the text of the line.
  [[noreturn]] void Fail(std::string_view what) const;

  // Reads value as a number for the word called word.
  [[nodiscard]] ULONGLONG Parse(std::string_view word,
                                std::string_view value) const;

  std::string source_;
  std::string kind_;
  std::vector<std::string> positional_;
  std::map<std::string, std::string, std::less<>> fields_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
