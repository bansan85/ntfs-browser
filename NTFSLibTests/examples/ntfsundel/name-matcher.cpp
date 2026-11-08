#include <cstddef>
#include <string_view>

#include <ntfs-browser/example/ntfsundel/name-matcher.h>
#include <ntfs-browser/namespace.h>

#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel {

namespace {

// Highest unit the table maps. A wchar_t above it is a code point of 32-bit
// wchar_t, and the table leaves such a point as it is.
constexpr wchar_t bmp_limit = 0xFFFF;

}  // namespace

NameMatcher::NameMatcher(const UpCase::Table& table) noexcept
    : table_(&table) {}

// Reports whether the two units are the same name unit under the table.
bool NameMatcher::SameUnit(wchar_t first, wchar_t second) const noexcept {
  if (first == second) {
    return true;
  }
  if (first > bmp_limit || second > bmp_limit) {
    return false;
  }
  return table_->Map(static_cast<char16_t>(first)) ==
         table_->Map(static_cast<char16_t>(second));
}

bool NameMatcher::Matches(std::wstring_view pattern,
                          std::wstring_view text) const noexcept {
  std::size_t p = 0;
  std::size_t t = 0;
  // The last '*' met, and the text position it resumes from.
  std::size_t star = std::wstring_view::npos;
  std::size_t resume = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == L'*') {
      star = p++;
      resume = t;
    } else if (p < pattern.size() &&
               (pattern[p] == L'?' || SameUnit(pattern[p], text[t]))) {
      ++p;
      ++t;
    } else if (star != std::wstring_view::npos) {
      p = star + 1;
      t = ++resume;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == L'*') {
    ++p;
  }
  return p == pattern.size();
}

bool NameMatcher::StartsWith(std::wstring_view text,
                             std::wstring_view prefix) const noexcept {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); i++) {
    if (!SameUnit(text[i], prefix[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel
