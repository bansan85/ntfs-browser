#pragma once

#include <memory>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "internal-export.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE {

// Decides whether an entry matches the Pattern of a scan. A matcher is built
// once, on the worker thread, after the volume is open, and called once for
// each entry before the entry reaches the exchange. A new kind of pattern is a
// new subclass and a new case in MakeMatcher(): no engine changes.
class NTFS_BROWSER_EXPORT_TESTS_ONLY Matcher {
 public:
  // target says which of the two texts of Matches() the subclass looks at.
  explicit Matcher(MatchTarget target) noexcept;
  Matcher(const Matcher&) = delete;
  Matcher& operator=(const Matcher&) = delete;
  Matcher(Matcher&&) = delete;
  Matcher& operator=(Matcher&&) = delete;
  virtual ~Matcher() = default;

  // Whether the entry with this name and this full path matches. path is empty
  // when the scan did not build it, which only happens for MatchTarget::Name.
  [[nodiscard]] bool Matches(std::u16string_view name,
                             std::u16string_view path) const;

 protected:
  // Whether text, the name or the path as the target chose, matches.
  [[nodiscard]] virtual bool MatchesText(std::u16string_view text) const = 0;

 private:
  MatchTarget target_;
};

// The matcher for pattern, folding case through table unless the pattern is
// case sensitive. table MUST outlive the matcher. Returns null, after a log
// line, when the pattern cannot be used: the kind is not available, or the text
// does not compile. The kind None has no matcher: do not ask for it.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<Matcher>
    MakeMatcher(const Pattern& pattern, const UpCase::Table& table);

}  // namespace NTFS_BROWSER_NAMESPACE
