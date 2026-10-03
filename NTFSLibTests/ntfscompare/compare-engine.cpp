#include "compare-engine.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstdio>
#include <optional>
#include <set>
#include <utility>

namespace NtfsCompare
{

namespace
{

// First code point past ASCII: Narrow() keeps only below it.
constexpr wchar_t kAsciiLimit = 128;

std::string FormatValue(ULONGLONG value) { return std::to_string(value); }
std::string FormatValue(bool value) { return value ? "true" : "false"; }

// ASCII-safe narrowing for a report line. Names round-trip correctly through
// the wide Listing key every method shares; this is display only.
std::string Narrow(const std::wstring& wide)
{
  std::string out;
  out.reserve(wide.size());
  for (wchar_t const character : wide)
  {
    out.push_back((character > 0 && character < kAsciiLimit)
                      ? static_cast<char>(character)
                      : '?');
  }
  return out;
}

// Calls visit(member pointer, report name) once per compared Entry field.
template <typename Visitor>
void ForEachComparedField(Visitor&& visit)
{
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
                                Report& report)
{
  std::optional<T> found;
  bool disagree = false;
  for (const Entry* source : sources)
  {
    const std::optional<T>& value = source->*member;
    if (!value)
    {
      continue;
    }
    if (!found)
    {
      found = value;
    }
    else if (*value != *found)
    {
      disagree = true;
    }
  }
  if (disagree)
  {
    report.findings.push_back(
        {"LIB-MISMATCH", "library (full-cache/no-cache/mft-tree)", path, field,
         "(no single value)", "the three library methods disagree"});
    return std::nullopt;
  }
  return found;
}

}  // namespace

Listing CompareLibraryMethods(const Listing& fullCache, const Listing& noCache,
                              const Listing& mftTree, Report& report)
{
  Listing reference;

  struct Source
  {
    const char* name;
    const Listing* listing;
  };
  const std::array<Source, 3> sources{{{"full-cache", &fullCache},
                                       {"no-cache", &noCache},
                                       {"mft-tree", &mftTree}}};

  std::set<std::wstring> allPaths;
  for (const Source& source : sources)
  {
    for (const auto& [path, entry] : *source.listing)
    {
      allPaths.insert(path);
    }
  }

  MethodStats stats{.name = "library"};

  for (const std::wstring& path : allPaths)
  {
    std::vector<const Entry*> present;
    for (const Source& source : sources)
    {
      const auto iterator = source.listing->find(path);
      if (iterator == source.listing->end())
      {
        report.findings.push_back({"LIB-MISSING", source.name, path, "", "",
                                   "present in the other library methods"});
        stats.missing++;
      }
      else
      {
        present.push_back(&iterator->second);
      }
    }

    if (present.empty())
    {
      continue;  // Unreachable: path came from at least one listing.
    }
    stats.compared_entries++;

    Entry ref;
    ref.is_directory = present.front()->is_directory;
    for (const Entry* entry : present)
    {
      if (entry->is_directory != ref.is_directory)
      {
        report.findings.push_back({"LIB-MISMATCH",
                                   "library (full-cache/no-cache/mft-tree)",
                                   path, "Type", "(no single value)",
                                   "the three library methods disagree"});
        stats.mismatched_fields++;
        break;
      }
    }

    ForEachComparedField(
        [&](auto member, const char* fieldName)
        {
          ref.*member =
              ReconcileField(present, fieldName, path, member, report);
        });

    reference.emplace(path, ref);
  }

  report.stats.push_back(stats);
  return reference;
}

void CompareAgainstReference(const std::string& methodName,
                             const Listing& reference, const Listing& candidate,
                             Report& report)
{
  MethodStats stats{.name = methodName};

  for (const auto& [path, refEntry] : reference)
  {
    const auto iterator = candidate.find(path);
    if (iterator == candidate.end())
    {
      report.findings.push_back({"MISSING", methodName, path, "", "", ""});
      stats.missing++;
      continue;
    }
    stats.compared_entries++;
    const Entry& cand = iterator->second;

    if (cand.is_directory != refEntry.is_directory)
    {
      report.findings.push_back({"MISMATCH", methodName, path, "Type",
                                 refEntry.is_directory ? "DIR" : "FILE",
                                 cand.is_directory ? "DIR" : "FILE"});
      stats.mismatched_fields++;
    }

    ForEachComparedField(
        [&](auto member, const char* fieldName)
        {
          const auto& refValue = refEntry.*member;
          const auto& candValue = cand.*member;
          if (refValue && candValue && *refValue != *candValue)
          {
            report.findings.push_back({"MISMATCH", methodName, path, fieldName,
                                       FormatValue(*refValue),
                                       FormatValue(*candValue)});
            stats.mismatched_fields++;
          }
        });
  }

  for (const auto& [path, entry] : candidate)
  {
    if (!reference.contains(path))
    {
      report.findings.push_back({"EXTRA", methodName, path, "", "", ""});
      stats.extra++;
    }
  }

  report.stats.push_back(stats);
}

bool PrintReport(const Report& report)
{
  for (const Finding& finding : report.findings)
  {
    if (finding.field.empty())
    {
      std::printf("[%s] %s: \"%s\"", finding.kind.c_str(),
                  finding.method.c_str(), Narrow(finding.path).c_str());
      if (!finding.actual.empty())
      {
        std::printf(" (%s)", finding.actual.c_str());
      }
      std::printf("\n");
    }
    else
    {
      std::printf("[%s] %s: \"%s\" %s: expected=%s actual=%s\n",
                  finding.kind.c_str(), finding.method.c_str(),
                  Narrow(finding.path).c_str(), finding.field.c_str(),
                  finding.expected.c_str(), finding.actual.c_str());
    }
  }

  std::printf("\n");
  for (const MethodStats& method_stats : report.stats)
  {
    std::printf(
        "%-12s compared=%zu missing=%zu extra=%zu mismatched_fields=%zu\n",
        method_stats.name.c_str(), method_stats.compared_entries,
        method_stats.missing, method_stats.extra,
        method_stats.mismatched_fields);
  }

  return !report.findings.empty();
}

}  // namespace NtfsCompare
