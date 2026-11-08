#include "search/matcher.h"

#include <memory>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>

#include "log/ntfs-common.h"
#include "search/case-folder.h"
#include "search/exact-matcher.h"
#if defined(NTFS_BROWSER_ENABLE_REGEX)
  #include "search/regex-matcher.h"
#endif
#include "search/wildcard-matcher.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

Matcher::Matcher(MatchTarget target) noexcept : target_(target) {}

bool Matcher::Matches(std::u16string_view name,
                      std::u16string_view path) const {
  return MatchesText(target_ == MatchTarget::FullPath ? path : name);
}

bool RegexAvailable() noexcept {
#if defined(NTFS_BROWSER_ENABLE_REGEX)
  return true;
#else
  return false;
#endif
}

std::unique_ptr<Matcher> MakeMatcher(const Pattern& pattern,
                                     const UpCase::Table& table) {
  const CaseFolder folder(pattern.case_sensitive ? nullptr : &table);
  switch (pattern.kind) {
    case PatternKind::Exact:
      return std::make_unique<ExactMatcher>(pattern.target, pattern.text,
                                            folder);
    case PatternKind::Wildcard:
      return std::make_unique<WildcardMatcher>(pattern.target, pattern.text,
                                               folder);
    case PatternKind::Regex:
#if defined(NTFS_BROWSER_ENABLE_REGEX)
      return RegexMatcher::Make(pattern, folder);
#else
      Log::Error("regex support is not compiled in");
      return nullptr;
#endif
    case PatternKind::None:
      break;
  }
  Log::Error("The scan has no pattern to match");
  return nullptr;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
