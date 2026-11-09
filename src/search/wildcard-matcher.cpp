#include "search/wildcard-matcher.h"

#include <cstddef>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "search/case-folder.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace {

// The wildcard that matches any run of characters, and the one that matches a
// single character.
constexpr char16_t any_run = u'*';
constexpr char16_t any_one = u'?';

// First and last unit of the high and of the low surrogates.
constexpr char16_t high_surrogate_first = 0xD800;
constexpr char16_t high_surrogate_last = 0xDBFF;
constexpr char16_t low_surrogate_first = 0xDC00;
constexpr char16_t low_surrogate_last = 0xDFFF;

// Units that the character at index of text takes: 2 for a surrogate pair, else
// 1, an unpaired surrogate included. index MUST be below the size of text.
std::size_t CharacterLength(std::u16string_view text, std::size_t index) {
  const bool pair = index + 1 < text.size() &&
                    text[index] >= high_surrogate_first &&
                    text[index] <= high_surrogate_last &&
                    text[index + 1] >= low_surrogate_first &&
                    text[index + 1] <= low_surrogate_last;
  return pair ? 2 : 1;
}

}  // namespace

WildcardMatcher::WildcardMatcher(MatchTarget target, std::u16string_view text,
                                 const CaseFolder& folder)
    : Matcher(target), folder_(folder), pattern_(folder.Fold(text)) {}

bool WildcardMatcher::MatchesText(std::u16string_view text) const {
  if (pattern_.empty()) {
    return false;
  }
  const std::u16string_view pattern = pattern_;
  std::size_t p = 0;
  std::size_t t = 0;
  // The last '*' met, and the position of the text it resumes from. When a
  // later unit fails, the star takes one more character and the match goes on
  // after it: an earlier star never needs to take more than the last one did.
  std::size_t star = std::u16string_view::npos;
  std::size_t resume = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == any_run) {
      star = p++;
      resume = t;
    } else if (p < pattern.size() && pattern[p] == any_one) {
      ++p;
      t += CharacterLength(text, t);
    } else if (p < pattern.size() && folder_.Fold(text[t]) == pattern[p]) {
      ++p;
      ++t;
    } else if (star != std::u16string_view::npos) {
      p = star + 1;
      resume += CharacterLength(text, resume);
      t = resume;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == any_run) {
    ++p;
  }
  return p == pattern.size();
}

}  // namespace NTFS_BROWSER_NAMESPACE
