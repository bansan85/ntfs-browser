#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// A name of exactly units code units: fill repeated, then tag as the last one.
// Names of different tags differ, so a path made of them shows which level a
// wrong piece came from.
[[nodiscard]] std::wstring FillerName(std::size_t units, wchar_t fill,
                                      wchar_t tag);

// A name a volume holds and the tools have to carry through the parse, the
// collation, the UTF-8 conversion and the printing.
struct ExoticName {
  // What the name is, for a failure message.
  std::string_view label;
  // The name as the library hands it out: the UTF-16 units of the record, with
  // a surrogate pair merged where wchar_t is 32 bits wide.
  std::wstring name;
  // The UTF-8 every tool prints for it: an unpaired surrogate keeps its own
  // three-byte form (WTF-8), a U+0000 is a NUL byte, nothing is dropped.
  std::string utf8;
};

// Every kind of exotic name: a character past the BMP, unpaired surrogates in
// each position, combining marks, zero-width and bidirectional controls,
// noncharacters, an embedded U+0000, the separators of a path, and a trailing
// dot and space.
[[nodiscard]] const std::vector<ExoticName>& ExoticNames();

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
