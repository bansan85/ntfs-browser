#include "compare-engine.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "console.h"

namespace NtfsCompare {

namespace {

// First code point past ASCII: Narrow() keeps only below it.
constexpr wchar_t ascii_limit = 128;

// Method name of the merged library reference. Every other method is an OS
// candidate.
constexpr std::string_view library_method_name = "library";

// Kind of the informative finding for a live file only MftTree reaches.
constexpr std::string_view unindexed_kind = "UNINDEXED";

std::string FormatValue(ULONGLONG value) { return std::to_string(value); }

std::string FormatValue(bool value) { return value ? "true" : "false"; }

// ASCII-safe narrowing for a report line. Names round-trip correctly through
// the wide Listing key every method shares; this is display only.
std::string Narrow(const std::wstring& wide) {
  std::string out;
  out.reserve(wide.size());
  for (const wchar_t character : wide) {
    out.push_back((character > 0 && character < ascii_limit)
                      ? static_cast<char>(character)
                      : '?');
  }
  return out;
}

// Calls visit(member pointer, report name) once per compared Entry field.
template <typename Visitor>
void ForEachComparedField(const Visitor& visit) {
  visit(&Entry::logical_size, "LogicalSize");
  visit(&Entry::physical_size, "PhysicalSize");
  visit(&Entry::creation_time_utc, "CreationTimeUtc");
  visit(&Entry::read_only, "ReadOnly");
  visit(&Entry::hidden, "Hidden");
  visit(&Entry::system, "System");
  visit(&Entry::archive, "Archive");
  visit(&Entry::compressed, "Compressed");
  visit(&Entry::encrypted, "Encrypted");
  visit(&Entry::sparse, "Sparse");
}

// Reconciles one field across every source that names this path, recording a
// LIB-MISMATCH and dropping the field to nullopt on any disagreement.
template <typename T>
std::optional<T> ReconcileField(const std::vector<const Entry*>& sources,
                                const char* field, const std::wstring& path,
                                std::optional<T> Entry::* member,
                                Report& report) {
  std::optional<T> found;
  bool disagree = false;
  for (const Entry* source : sources) {
    const std::optional<T>& value = source->*member;
    if (!value) {
      continue;
    }
    if (!found) {
      found = value;
    } else if (*value != *found) {
      disagree = true;
    }
  }
  if (disagree) {
    report.findings.push_back(
        {"LIB-MISMATCH", "library (full-cache/no-cache/mft-tree)", path, field,
         "(no single value)", "the three library methods disagree"});
    return std::nullopt;
  }
  return found;
}

// Compares one candidate entry with the reference entry at path, reporting
// each field that both sides provide and that differs.
void CompareCandidateEntry(const std::string& method_name,
                           const std::wstring& path, const Entry& ref_entry,
                           const Entry& cand, Report& report,
                           MethodStats& stats) {
  if (cand.is_directory != ref_entry.is_directory) {
    report.findings.push_back({"MISMATCH", method_name, path, "Type",
                               ref_entry.is_directory ? "DIR" : "FILE",
                               cand.is_directory ? "DIR" : "FILE"});
    stats.mismatched_fields++;
  }

  ForEachComparedField([&](auto member, const char* field_name) {
    const auto& ref_value = ref_entry.*member;
    const auto& cand_value = cand.*member;
    if (ref_value && cand_value && *ref_value != *cand_value) {
      report.findings.push_back({"MISMATCH", method_name, path, field_name,
                                 FormatValue(*ref_value),
                                 FormatValue(*cand_value)});
      stats.mismatched_fields++;
    }
  });
}

// Reports each candidate path that the reference does not have.
void ReportExtras(const std::string& method_name, const Listing& reference,
                  const Listing& candidate, Report& report,
                  MethodStats& stats) {
  for (const auto& [path, entry] : candidate) {
    if (!reference.contains(path)) {
      report.findings.push_back({"EXTRA", method_name, path, "", "", ""});
      stats.extra++;
    }
  }
}

}  // namespace

Listing CompareLibraryMethods(const Listing& full_cache,
                              const Listing& no_cache, const Listing& mft_tree,
                              Report& report) {
  Listing reference;

  struct Source {
    const char* name;
    const Listing* listing;
  };

  const std::array<Source, 3> sources{{{"full-cache", &full_cache},
                                       {"no-cache", &no_cache},
                                       {"mft-tree", &mft_tree}}};

  std::set<std::wstring> all_paths;
  for (const Source& source : sources) {
    for (const auto& [path, entry] : *source.listing) {
      all_paths.insert(path);
    }
  }

  MethodStats stats{.name = std::string(library_method_name)};

  for (const std::wstring& path : all_paths) {
    // NTFS names a file only through its directory's index, so a live file
    // that only MftTree reaches is not a miss.
    const bool indexed = full_cache.contains(path) || no_cache.contains(path);
    std::vector<const Entry*> present;
    for (const Source& source : sources) {
      const auto iterator = source.listing->find(path);
      if (iterator != source.listing->end()) {
        present.push_back(&iterator->second);
      } else if (indexed) {
        report.findings.push_back({"LIB-MISSING", source.name, path, "", "",
                                   "present in the other library methods"});
        stats.missing++;
      }
    }

    if (!indexed) {
      report.findings.push_back({std::string(unindexed_kind), "mft-tree", path,
                                 "", "",
                                 "not in the full-cache or no-cache index"});
      report.unindexed_paths.insert(path);
      stats.unindexed++;
    }

    if (present.empty()) {
      continue;  // Unreachable: path came from at least one listing.
    }
    stats.compared_entries++;

    Entry ref;
    ref.is_directory = present.front()->is_directory;
    for (const Entry* entry : present) {
      if (entry->is_directory != ref.is_directory) {
        report.findings.push_back({"LIB-MISMATCH",
                                   "library (full-cache/no-cache/mft-tree)",
                                   path, "Type", "(no single value)",
                                   "the three library methods disagree"});
        stats.mismatched_fields++;
        break;
      }
    }

    ForEachComparedField([&](auto member, const char* field_name) {
      ref.*member = ReconcileField(present, field_name, path, member, report);
    });

    reference.emplace(path, ref);
  }

  report.stats.push_back(stats);
  return reference;
}

void CompareAgainstReference(const std::string& method_name,
                             const Listing& reference, const Listing& candidate,
                             Report& report) {
  MethodStats stats{.name = method_name};

  for (const auto& [path, ref_entry] : reference) {
    if (report.unindexed_paths.contains(path)) {
      stats.unindexed++;
      continue;
    }
    const auto iterator = candidate.find(path);
    if (iterator == candidate.end()) {
      report.findings.push_back({"MISSING", method_name, path, "", "", ""});
      stats.missing++;
      continue;
    }
    stats.compared_entries++;
    CompareCandidateEntry(method_name, path, ref_entry, iterator->second,
                          report, stats);
  }

  ReportExtras(method_name, reference, candidate, report, stats);
  report.stats.push_back(stats);
}

bool PrintReport(const Report& report) {
  bool has_failure = false;
  for (const Finding& finding : report.findings) {
    has_failure = has_failure || finding.kind != unindexed_kind;
    if (finding.field.empty()) {
      PrintOut("[{}] {}: \"{}\"", finding.kind, finding.method,
               Narrow(finding.path));
      if (!finding.actual.empty()) {
        PrintOut(" ({})", finding.actual);
      }
      PrintOut("\n");
    } else {
      PrintOut("[{}] {}: \"{}\" {}: expected={} actual={}\n", finding.kind,
               finding.method, Narrow(finding.path), finding.field,
               finding.expected, finding.actual);
    }
  }

  PrintOut("\n");
  for (const MethodStats& method_stats : report.stats) {
    PrintOut(
        "{:<12} compared={} missing={} extra={} mismatched_fields={} "
        "unindexed={}\n",
        method_stats.name, method_stats.compared_entries, method_stats.missing,
        method_stats.extra, method_stats.mismatched_fields,
        method_stats.unindexed);
    if (method_stats.name != library_method_name &&
        method_stats.unindexed > 0) {
      PrintOut("{} unindexed entries excluded from the {} comparison\n",
               method_stats.unindexed, method_stats.name);
    }
  }

  return has_failure;
}

}  // namespace NtfsCompare
