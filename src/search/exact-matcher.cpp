#include "search/exact-matcher.h"

#include <cstddef>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "search/case-folder.h"

namespace NTFS_BROWSER_NAMESPACE {

ExactMatcher::ExactMatcher(MatchTarget target, std::u16string_view text,
                           const CaseFolder& folder)
    : Matcher(target), folder_(folder), pattern_(folder.Fold(text)) {}

bool ExactMatcher::MatchesText(std::u16string_view text) const {
  if (pattern_.empty() || text.size() != pattern_.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (folder_.Fold(text[i]) != pattern_[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE
