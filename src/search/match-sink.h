#pragma once

#include <memory>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "internal-export.h"
#include "search/engine.h"
#include "search/field-filter.h"
#include "search/matcher.h"
#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// The sink an engine gives its entries to. It passes on to the real sink the
// entries that the options select, and drops the others, so that an entry that
// does not match is never copied into the exchange. The selection is the tests
// of Options::filter on the fields (FieldFilter) and the name pattern, joined
// with AND. The cheap tests of the fields run first, so that a regular
// expression only runs on the entries that the numbers kept.
//
// An engine builds it once, after it opened the volume and before the long
// work, and gives it to the walk in place of the sink. It never changes what an
// engine walks: a directory that does not match is still entered.
class NTFS_BROWSER_EXPORT_TESTS_ONLY MatchingSink final : public EntrySink {
 public:
  // Builds the sink in front of inner, which MUST outlive it, with the pattern
  // of options folded through table, and the filter checked against engine, the
  // engine that gives the entries. Returns null, after a log line, when the
  // options cannot be used (see MakeMatcher() and FieldFilter::Make()): the
  // scan then fails. A pattern on the full path needs Options::full_path: Scan
  // sets it before it makes the engine.
  [[nodiscard]] static std::unique_ptr<MatchingSink>
      Make(EntrySink& inner, const Options& options, const UpCase::Table& table,
           EngineKind engine);

  // Passes entry on when it is selected.
  void Add(const Entry& entry) override;

  // Passes the percent on: the progress of a scan does not depend on what it
  // finds.
  void SetPercent(double percent) override;

 private:
  MatchingSink(EntrySink& inner, FieldFilter filter,
               std::unique_ptr<Matcher> matcher);

  // Whether entry is selected.
  [[nodiscard]] bool Accepts(const Entry& entry) const;

  EntrySink& inner_;
  FieldFilter filter_;
  // Null for a scan without a pattern.
  std::unique_ptr<Matcher> matcher_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Search
