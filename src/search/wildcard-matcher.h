#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "internal-export.h"
#include "search/case-folder.h"
#include "search/matcher.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// PatternKind::Wildcard: '*' for any run, '?' for one character, see Pattern.
// The comparison keeps one resume point, the last '*', so a pattern costs at
// most the product of its length and the length of the text. It never tries the
// stars one against the other.
class NTFS_BROWSER_EXPORT_TESTS_ONLY WildcardMatcher final : public Matcher {
 public:
  // Folds text once, with folder. The wildcards map to themselves.
  WildcardMatcher(MatchTarget target, std::u16string_view text,
                  const CaseFolder& folder);

 protected:
  [[nodiscard]] bool MatchesText(std::u16string_view text) const override;

 private:
  CaseFolder folder_;
  // The pattern, folded. An empty pattern matches nothing.
  std::u16string pattern_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Search
