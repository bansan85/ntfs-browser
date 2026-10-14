#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "entry.h"

namespace NtfsCompare {

// One discrepancy the comparison found, ready to print.
struct Finding {
  // "LIB-MISMATCH", "LIB-MISSING", "LIB-EXTRA", "MISSING", "EXTRA",
  // "MISMATCH" or "UNINDEXED". UNINDEXED is informative, not a failure.
  std::string kind;
  std::string method;
  std::wstring path;
  // Empty for MISSING/EXTRA/LIB-MISSING/LIB-EXTRA, which concern a whole
  // entry, not one field.
  std::string field;
  std::string expected;
  std::string actual;
};

struct MethodStats {
  std::string name;
  size_t compared_entries = 0;
  size_t missing = 0;
  size_t extra = 0;
  size_t mismatched_fields = 0;
  // Library: entries only MftTree reaches. Candidate: those left out of the
  // comparison.
  size_t unindexed = 0;
};

struct Report {
  std::vector<Finding> findings;
  std::vector<MethodStats> stats;
  // Paths only MftTree reaches. Passe 2/3 leave them out of the comparison.
  std::set<std::wstring> unindexed_paths;
};

// Passe 1: cross-checks fullCache/noCache/mftTree pairwise on every path and
// field - all three read the same on-disk bytes through different code
// paths, so any disagreement is a library bug, not an artifact of some
// method's own limits. Returns the merged reference the later passes diff
// against: for a path/field, the value every source that has it agrees on.
// A disagreeing path/field is already reported here and left out of the
// reference, so it can't cascade into false positives downstream. A live
// file that only MftTree reaches is UNINDEXED: it stays in the reference, and
// is recorded in report.unindexed_paths.
[[nodiscard]] Listing CompareLibraryMethods(const Listing& full_cache,
                                            const Listing& no_cache,
                                            const Listing& mft_tree,
                                            Report& report);

// Passe 2/3: diffs candidate (std::filesystem, or the native OS API) against
// the Passe 1 reference, on every field both sides have a value for. A field
// candidate structurally never provides (std::nullopt on its side) is never
// compared, and so never reported. An UNINDEXED path is skipped entirely.
void CompareAgainstReference(const std::string& method_name,
                             const Listing& reference, const Listing& candidate,
                             Report& report);

// Prints every finding, then a per-method summary line, to stdout. Returns
// true if the report holds a finding other than UNINDEXED.
bool PrintReport(const Report& report);

}  // namespace NtfsCompare
