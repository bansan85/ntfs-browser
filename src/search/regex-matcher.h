#pragma once

#include <memory>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "internal-export.h"
#include "search/case-folder.h"
#include "search/matcher.h"

namespace NTFS_BROWSER_NAMESPACE {

// PatternKind::Regex: a PCRE2 regular expression, see Pattern. The expression
// is compiled once, in UTF-16 mode, and JIT compiled when the CPU allows it.
// The interpreter takes over when it does not. It is a search: the expression
// matches when it matches any part of the text.
//
// The PCRE2 state lives in the .cpp, so no PCRE2 header reaches the callers.
// Only a build with NTFS_BROWSER_ENABLE_REGEX compiles the class: MakeMatcher()
// never asks for it otherwise.
//
// Thread. The matcher reuses one match buffer and one JIT stack, so Matches()
// MUST be called from one thread only: the worker of the scan.
class NTFS_BROWSER_EXPORT_TESTS_ONLY RegexMatcher final : public Matcher {
 public:
  // Compiles pattern.text. Returns null, after a log line with the PCRE2
  // message and the position of the error, when the text is not a valid
  // expression. folder maps the units of each text before the match, unless the
  // pattern is case sensitive. The table behind it MUST outlive the matcher.
  // The limits of the pattern are set on the match: 0 stands for the default.
  [[nodiscard]] static std::unique_ptr<RegexMatcher>
      Make(const Pattern& pattern, const CaseFolder& folder);

  ~RegexMatcher() override;

 protected:
  // Whether the expression matches in text. A text on which PCRE2 gives up (a
  // limit, the JIT stack) is no match. The first one of the scan is logged.
  [[nodiscard]] bool MatchesText(std::u16string_view text) const override;

 private:
  // Every PCRE2 object and the reusable buffers.
  struct State;

  RegexMatcher(MatchTarget target, const CaseFolder& folder, bool fold,
               std::unique_ptr<State> state);

  CaseFolder folder_;
  // Whether MatchesText() folds the text through folder_ first.
  bool fold_;
  std::unique_ptr<State> state_;
};

}  // namespace NTFS_BROWSER_NAMESPACE
