#include "search/matcher.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/types.h>

#include "log/ntfs-common.h"
#include "search/case-folder.h"
#include "search/exact-matcher.h"
#if defined(NTFS_BROWSER_ENABLE_REGEX)
  #include "search/regex-matcher.h"
#endif
#include "search/wildcard-matcher.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE {

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

std::string EntryFlags(const Entry& entry) {
  // Each flag is one letter, in the order the declaration gives.
  const auto letter = [&entry](std::uint32_t bit, char set) {
    return (entry.attributes & bit) != 0 ? set : '-';
  };
  std::string flags;
  flags += entry.is_directory ? 'D' : '-';
  flags += letter(Attribute::read_only, 'R');
  flags += letter(Attribute::hidden, 'H');
  flags += letter(Attribute::system, 'S');
  flags += letter(Attribute::archive, 'A');
  flags += letter(Attribute::compressed, 'C');
  flags += letter(Attribute::encrypted, 'E');
  return flags;
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

}  // namespace NTFS_BROWSER_NAMESPACE
