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

std::string FormatValue(ULONGLONG v) { return std::to_string(v); }
std::string FormatValue(bool v) { return v ? "true" : "false"; }

// ASCII-safe narrowing for a report line. Names round-trip correctly through
// the wide Listing key every method shares; this is display only.
std::string Narrow(const std::wstring& w)
{
  std::string out;
  out.reserve(w.size());
  for (wchar_t const c : w)
  {
    out.push_back((c > 0 && c < kAsciiLimit) ? static_cast<char>(c) : '?');
  }
  return out;
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
  for (const Entry* s : sources)
  {
    const std::optional<T>& v = s->*member;
    if (!v)
    {
      continue;
    }
    if (!found)
    {
      found = v;
    }
    else if (*v != *found)
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
  for (const Source& s : sources)
  {
    for (const auto& [path, entry] : *s.listing)
    {
      allPaths.insert(path);
    }
  }

  MethodStats stats{.name = "library"};

  for (const std::wstring& path : allPaths)
  {
    std::vector<const Entry*> present;
    for (const Source& s : sources)
    {
      const auto it = s.listing->find(path);
      if (it == s.listing->end())
      {
        report.findings.push_back({"LIB-MISSING", s.name, path, "", "",
                                   "present in the other library methods"});
        stats.missing++;
      }
      else
      {
        present.push_back(&it->second);
      }
    }

    if (present.empty())
    {
      continue;  // Unreachable: path came from at least one listing.
    }
    stats.compared_entries++;

    Entry ref;
    ref.is_directory = present.front()->is_directory;
    for (const Entry* e : present)
    {
      if (e->is_directory != ref.is_directory)
      {
        report.findings.push_back({"LIB-MISMATCH",
                                   "library (full-cache/no-cache/mft-tree)",
                                   path, "Type", "(no single value)",
                                   "the three library methods disagree"});
        stats.mismatched_fields++;
        break;
      }
    }

#define NTFSCOMPARE_RECONCILE(member, field_name) \
  ref.member = ReconcileField(present, field_name, path, &Entry::member, report)
    NTFSCOMPARE_RECONCILE(logical_size, "LogicalSize");
    NTFSCOMPARE_RECONCILE(physical_size, "PhysicalSize");
    NTFSCOMPARE_RECONCILE(creation_time_utc, "CreationTimeUtc");
    NTFSCOMPARE_RECONCILE(read_only, "ReadOnly");
    NTFSCOMPARE_RECONCILE(hidden, "Hidden");
    NTFSCOMPARE_RECONCILE(system, "System");
    NTFSCOMPARE_RECONCILE(archive, "Archive");
    NTFSCOMPARE_RECONCILE(compressed, "Compressed");
    NTFSCOMPARE_RECONCILE(encrypted, "Encrypted");
    NTFSCOMPARE_RECONCILE(sparse, "Sparse");
#undef NTFSCOMPARE_RECONCILE

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
    const auto it = candidate.find(path);
    if (it == candidate.end())
    {
      report.findings.push_back({"MISSING", methodName, path, "", "", ""});
      stats.missing++;
      continue;
    }
    stats.compared_entries++;
    const Entry& cand = it->second;

    if (cand.is_directory != refEntry.is_directory)
    {
      report.findings.push_back({"MISMATCH", methodName, path, "Type",
                                 refEntry.is_directory ? "DIR" : "FILE",
                                 cand.is_directory ? "DIR" : "FILE"});
      stats.mismatched_fields++;
    }

#define NTFSCOMPARE_COMPARE(member, field_name)                            \
  if (refEntry.member && cand.member)                                      \
  {                                                                        \
    if (*refEntry.member != *cand.member)                                  \
    {                                                                      \
      report.findings.push_back({"MISMATCH", methodName, path, field_name, \
                                 FormatValue(*refEntry.member),            \
                                 FormatValue(*cand.member)});              \
      stats.mismatched_fields++;                                           \
    }                                                                      \
  }
    NTFSCOMPARE_COMPARE(logical_size, "LogicalSize")
    NTFSCOMPARE_COMPARE(physical_size, "PhysicalSize")
    NTFSCOMPARE_COMPARE(creation_time_utc, "CreationTimeUtc")
    NTFSCOMPARE_COMPARE(read_only, "ReadOnly")
    NTFSCOMPARE_COMPARE(hidden, "Hidden")
    NTFSCOMPARE_COMPARE(system, "System")
    NTFSCOMPARE_COMPARE(archive, "Archive")
    NTFSCOMPARE_COMPARE(compressed, "Compressed")
    NTFSCOMPARE_COMPARE(encrypted, "Encrypted")
    NTFSCOMPARE_COMPARE(sparse, "Sparse")
#undef NTFSCOMPARE_COMPARE
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
  for (const Finding& f : report.findings)
  {
    if (f.field.empty())
    {
      std::printf("[%s] %s: \"%s\"", f.kind.c_str(), f.method.c_str(),
                  Narrow(f.path).c_str());
      if (!f.actual.empty())
      {
        std::printf(" (%s)", f.actual.c_str());
      }
      std::printf("\n");
    }
    else
    {
      std::printf("[%s] %s: \"%s\" %s: expected=%s actual=%s\n", f.kind.c_str(),
                  f.method.c_str(), Narrow(f.path).c_str(), f.field.c_str(),
                  f.expected.c_str(), f.actual.c_str());
    }
  }

  std::printf("\n");
  for (const MethodStats& s : report.stats)
  {
    std::printf(
        "%-12s compared=%zu missing=%zu extra=%zu mismatched_fields=%zu\n",
        s.name.c_str(), s.compared_entries, s.missing, s.extra,
        s.mismatched_fields);
  }

  return !report.findings.empty();
}

}  // namespace NtfsCompare
