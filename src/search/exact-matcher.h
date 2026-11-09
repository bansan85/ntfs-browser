#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "internal-export.h"
#include "search/case-folder.h"
#include "search/matcher.h"

namespace NTFS_BROWSER_NAMESPACE {

// PatternKind::Exact: the whole text equals the pattern, unit by unit after the
// folder maps both.
class NTFS_BROWSER_EXPORT_TESTS_ONLY ExactMatcher final : public Matcher {
 public:
  // Folds text once, with folder.
  ExactMatcher(MatchTarget target, std::u16string_view text,
               const CaseFolder& folder);

 protected:
  [[nodiscard]] bool MatchesText(std::u16string_view text) const override;

 private:
  CaseFolder folder_;
  // The pattern, folded. An empty pattern matches nothing.
  std::u16string pattern_;
};

}  // namespace NTFS_BROWSER_NAMESPACE
