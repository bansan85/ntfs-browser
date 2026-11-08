#include "search/match-sink.h"

#include <memory>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "search/engine.h"
#include "search/matcher.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

std::unique_ptr<MatchingSink> MatchingSink::Make(EntrySink& inner,
                                                 const Options& options,
                                                 const UpCase::Table& table) {
  std::unique_ptr<Matcher> matcher;
  if (options.pattern.kind != PatternKind::None) {
    matcher = MakeMatcher(options.pattern, table);
    if (!matcher) {
      return nullptr;
    }
  }
  return std::unique_ptr<MatchingSink>(
      new MatchingSink(inner, std::move(matcher)));
}

MatchingSink::MatchingSink(EntrySink& inner, std::unique_ptr<Matcher> matcher)
    : inner_(inner), matcher_(std::move(matcher)) {}

void MatchingSink::Add(const Entry& entry) {
  if (Accepts(entry)) {
    inner_.Add(entry);
  }
}

void MatchingSink::SetPercent(double percent) { inner_.SetPercent(percent); }

bool MatchingSink::Accepts(const Entry& entry) const {
  return matcher_ == nullptr || matcher_->Matches(entry.name, entry.path);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
