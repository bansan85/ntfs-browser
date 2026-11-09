#pragma once

#include <cstdint>
#include <optional>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {

// What the filter of a scan asks of the rest of the scan, besides the tests
// themselves.
struct FilterNeeds {
  // The filter tests a field that only the MftList engine gives.
  bool mft_list_only{false};
  // The filter tests a field that only the directory engines give.
  bool directory_only{false};
  // The filter needs Options::deleted_data_state: the data state or the
  // cluster counts of the deleted files.
  bool deleted_data_state{false};
  // The filter lets deleted entries pass only, so a scan that does not list
  // them finds nothing.
  bool deleted_entries_only{false};
};

// What filter asks of the scan.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY FilterNeeds
    NeedsOf(const Filter& filter);

// The tests of Options::filter on the fields of an entry. The pattern is not
// part of it: MatchingSink asks the matcher apart, after the cheap tests of
// this class, so that a regular expression only runs on the entries that the
// numbers have kept.
class NTFS_BROWSER_EXPORT_TESTS_ONLY FieldFilter {
 public:
  // Checks options.filter against the engine that will give the entries and
  // builds the tests. Returns nothing, after a log line, when the filter cannot
  // be used: a range with min above max, an attribute bit that does not exist,
  // or a field that the engine does not give (see Filter). Logs a warning, and
  // still builds the tests, when the filter can match deleted entries only and
  // the options do not list them. engine is never Auto.
  [[nodiscard]] static std::optional<FieldFilter> Make(const Options& options,
                                                       EngineKind engine);

  // Whether any test is set. A filter with none passes every entry.
  [[nodiscard]] bool IsActive() const noexcept { return active_; }

  // Whether entry passes every test that is set. Tests the cheap fields first.
  [[nodiscard]] bool Matches(const Entry& entry) const;

 private:
  // Takes a validated filter and prepares the lookups.
  explicit FieldFilter(const Filter& filter);

  // Whether entry passes the tests on its size, its times and its attributes.
  [[nodiscard]] bool MatchesNumbers(const Entry& entry) const;

  // Whether entry passes the tests on its state: the standing, the data state
  // and the clusters.
  [[nodiscard]] bool MatchesState(const Entry& entry) const;

  Filter filter_;
  bool active_{false};
  // One bit for each value of Standing that filter_.standing_in lists.
  std::uint32_t standing_bits_{0};
  // One bit for each value of DataState that filter_.data_state_in lists.
  std::uint32_t data_state_bits_{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE
