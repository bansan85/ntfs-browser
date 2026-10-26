#include "compare-engine.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "console.h"
#include "text-convert.h"

#ifndef _WIN32
  #include "driver-names.h"
#endif

namespace NtfsCompare {

namespace {

// Method name of the merged library reference. Every other method is an OS
// candidate.
constexpr std::string_view library_method_name = "library";

// Name of the library method that reads the $MFT records, whose values the
// reference takes for everything the directory index also copies.
constexpr std::string_view mft_tree_source = "mft-tree";

// Kind of the informative finding for a live file only MftTree reaches.
constexpr std::string_view unindexed_kind = "UNINDEXED";

std::string FormatValue(ULONGLONG value) { return std::to_string(value); }

std::string FormatValue(bool value) { return value ? "true" : "false"; }

// Narrowing for a report line. Names round-trip correctly through the wide
// Listing key every method shares; this is display only. A name prints as
// UTF-8, like the other tools' output.
std::string Narrow(const std::wstring& wide) {
  return NtfsDevice::WideToUtf8(wide);
}

// Calls visit(member pointer, report name) once per compared Entry field. The
// access time is not one: listing a directory updates it, in the $MFT record
// and, later, in the directory index, and the Windows API reports the index
// copy. No two reads of it agree, so a comparison could not be stable.
template <typename Visitor>
void ForEachComparedField(const Visitor& visit) {
  visit(&Entry::logical_size, "LogicalSize");
  visit(&Entry::physical_size, "PhysicalSize");
  visit(&Entry::creation_time_utc, "CreationTimeUtc");
  visit(&Entry::modification_time_utc, "ModificationTimeUtc");
  visit(&Entry::change_time_utc, "ChangeTimeUtc");
  visit(&Entry::read_only, "ReadOnly");
  visit(&Entry::hidden, "Hidden");
  visit(&Entry::system, "System");
  visit(&Entry::archive, "Archive");
  visit(&Entry::compressed, "Compressed");
  visit(&Entry::encrypted, "Encrypted");
  visit(&Entry::sparse, "Sparse");
}

// An entry and the library method that produced it.
struct NamedEntry {
  const char* source;
  const Entry* entry;
};

// Reconciles one field across every source that names this path, recording a
// LIB-MISMATCH and dropping the field to nullopt on any disagreement. The
// finding lists what each method reported, so a disagreement between the
// directory index and the MFT record can be told from one between two caches.
template <typename T>
std::optional<T> ReconcileField(const std::vector<NamedEntry>& sources,
                                const char* field, const std::wstring& path,
                                std::optional<T> Entry::* member,
                                Report& report) {
  std::optional<T> found;
  bool disagree = false;
  std::string values;
  for (const NamedEntry& source : sources) {
    const std::optional<T>& value = source.entry->*member;
    if (!value) {
      continue;
    }
    values += std::string(values.empty() ? "" : " ") + source.source + "=" +
              FormatValue(*value);
    if (!found) {
      found = value;
    } else if (*value != *found) {
      disagree = true;
    }
  }
  if (disagree) {
    report.findings.push_back({"LIB-MISMATCH", "library (full-cache/no-cache)",
                               path, field, "(no single value)", values});
    return std::nullopt;
  }
  return found;
}

// Compares one candidate entry with the reference entry at path, reporting
// each field that both sides provide and that differs. stats names the method.
void CompareCandidateEntry(const std::wstring& path, const Entry& ref_entry,
                           const Entry& cand, Report& report,
                           MethodStats& stats) {
  if (cand.is_directory != ref_entry.is_directory) {
    report.findings.push_back({"MISMATCH", stats.name, path, "Type",
                               ref_entry.is_directory ? "DIR" : "FILE",
                               cand.is_directory ? "DIR" : "FILE"});
    stats.mismatched_fields++;
  }

  ForEachComparedField([&](auto member, const char* field_name) {
    const auto& ref_value = ref_entry.*member;
    const auto& cand_value = cand.*member;
    if (ref_value && cand_value && *ref_value != *cand_value) {
      report.findings.push_back({"MISMATCH", stats.name, path, field_name,
                                 FormatValue(*ref_value),
                                 FormatValue(*cand_value)});
      stats.mismatched_fields++;
    }
  });
}

// Reports each candidate path that the reference does not have. stats names
// the method.
void ReportExtras(const Listing& reference, const Listing& candidate,
                  Report& report, MethodStats& stats) {
  for (const auto& [path, entry] : candidate) {
    if (!reference.contains(path)) {
      report.findings.push_back({"EXTRA", stats.name, path, "", "", ""});
      stats.extra++;
    }
  }
}

// A library method and what it listed.
struct LibrarySource {
  const char* name;
  const Listing* listing;
};

using LibrarySources = std::array<LibrarySource, 3>;

// Every path that at least one library method lists.
std::set<std::wstring> ListedPaths(const LibrarySources& sources) {
  std::set<std::wstring> paths;
  for (const LibrarySource& source : sources) {
    for (const auto& [path, entry] : *source.listing) {
      paths.insert(path);
    }
  }
  return paths;
}

// True when a directory index lists path. NTFS names a file only through its
// directory's index, so a live file that only MftTree reaches is not a miss.
bool IsIndexed(const LibrarySources& sources, const std::wstring& path) {
  return std::ranges::any_of(sources, [&](const LibrarySource& source) {
    return std::string_view(source.name) != mft_tree_source &&
           source.listing->contains(path);
  });
}

// The entry of path in each library method that lists it. An indexed path that
// a method lacks is a LIB-MISSING finding.
std::vector<NamedEntry> EntriesAt(const LibrarySources& sources,
                                  const std::wstring& path, bool indexed,
                                  Report& report, MethodStats& stats) {
  std::vector<NamedEntry> present;
  for (const LibrarySource& source : sources) {
    const auto iterator = source.listing->find(path);
    if (iterator != source.listing->end()) {
      present.push_back({.source = source.name, .entry = &iterator->second});
    } else if (indexed) {
      report.findings.push_back({"LIB-MISSING", source.name, path, "", "",
                                 "present in the other library methods"});
      stats.missing++;
    }
  }
  return present;
}

// Builds the reference entry of path out of the entries the library methods
// listed, reporting a LIB-MISMATCH where they disagree.
Entry MakeReferenceEntry(const std::wstring& path,
                         const std::vector<NamedEntry>& present, Report& report,
                         MethodStats& stats) {
  Entry ref;
  ref.is_directory = present.front().entry->is_directory;
  const bool type_disagrees =
      std::ranges::any_of(present, [&](const NamedEntry& named) {
        return named.entry->is_directory != ref.is_directory;
      });
  if (type_disagrees) {
    report.findings.push_back(
        {"LIB-MISMATCH", "library (full-cache/no-cache/mft-tree)", path, "Type",
         "(no single value)", "the three library methods disagree"});
    stats.mismatched_fields++;
  }

  // The directory index keeps its own copy of the sizes, times and
  // attributes, and NTFS updates it late (the access time almost never, a
  // file just created or written not yet). The $MFT record is the source:
  // the reference takes its values, and the index copies only have to agree
  // between the two cache strategies.
  std::vector<NamedEntry> index_sources;
  const Entry* record = nullptr;
  for (const NamedEntry& named : present) {
    if (std::string_view(named.source) == mft_tree_source) {
      record = named.entry;
    } else {
      index_sources.push_back(named);
    }
  }
  ForEachComparedField([&](auto member, const char* field_name) {
    static_cast<void>(
        ReconcileField(index_sources, field_name, path, member, report));
    if (record != nullptr) {
      ref.*member = record->*member;
    }
  });
  return ref;
}

}  // namespace

Listing CompareLibraryMethods(const Listing& full_cache,
                              const Listing& no_cache, const Listing& mft_tree,
                              Report& report) {
  Listing reference;
  const LibrarySources sources{{{"full-cache", &full_cache},
                                {"no-cache", &no_cache},
                                {"mft-tree", &mft_tree}}};
  MethodStats stats{.name = std::string(library_method_name)};

  for (const std::wstring& path : ListedPaths(sources)) {
    const bool indexed = IsIndexed(sources, path);
    const std::vector<NamedEntry> present =
        EntriesAt(sources, path, indexed, report, stats);

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
    reference.emplace(path, MakeReferenceEntry(path, present, report, stats));
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
    CompareCandidateEntry(path, ref_entry, iterator->second, report, stats);
  }

  ReportExtras(reference, candidate, report, stats);
  report.stats.push_back(stats);
}

namespace {

// Example findings the field summary shows under each of its lines.
constexpr size_t summary_examples = 3;

// Prints how many findings each kind, method and field has, with a few
// examples of each, so a run over a large volume can be read at a glance.
void PrintFieldSummary(const Report& report) {
  struct Group {
    size_t count = 0;
    std::vector<const Finding*> examples;
  };

  std::map<std::tuple<std::string, std::string, std::string>, Group> groups;
  for (const Finding& finding : report.findings) {
    if (finding.kind == unindexed_kind) {
      continue;
    }
    Group& group = groups[{finding.kind, finding.method, finding.field}];
    group.count++;
    if (group.examples.size() < summary_examples) {
      group.examples.push_back(&finding);
    }
  }
  if (groups.empty()) {
    return;
  }

  PrintOut("\nDifferences by field:\n");
  for (const auto& [key, group] : groups) {
    const auto& [kind, method, field] = key;
    PrintOut("{:>8}  [{}] {} {}\n", group.count, kind, method,
             field.empty() ? "(whole entry)" : field);
    for (const Finding* example : group.examples) {
      PrintOut("            e.g. \"{}\" expected={} actual={}\n",
               Narrow(example->path), example->expected, example->actual);
    }
  }
}

}  // namespace

bool PrintReport(const Report& report, bool details) {
  bool has_failure = false;
  for (const Finding& finding : report.findings) {
    has_failure = has_failure || finding.kind != unindexed_kind;
    if (!details) {
      continue;
    }
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

  PrintFieldSummary(report);
  return has_failure;
}

#ifndef _WIN32

bool OsNamesAreComparable(const Listing& reference,
                          const std::string& mount_fstype) {
  const bool needs_mapping =
      std::ranges::any_of(reference, [](const auto& listed) {
        return NtfsDevice::NeedsDriverMapping(listed.first);
      });
  if (!needs_mapping || NtfsDevice::IsDriverMappingKnown(mount_fstype)) {
    return true;
  }

  PrintErr(
      "A name has no UTF-8 form, and the mapping of the \"{}\" driver that "
      "mounts this volume is not known: the operating-system comparison "
      "cannot proceed.\n",
      mount_fstype);
  return false;
}

#endif  // !_WIN32

}  // namespace NtfsCompare
