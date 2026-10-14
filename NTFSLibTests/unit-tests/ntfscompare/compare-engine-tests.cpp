#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "compare-engine.h"
#include "entry.h"

using NtfsCompare::CompareAgainstReference;
using NtfsCompare::CompareLibraryMethods;
using NtfsCompare::Entry;
using NtfsCompare::Finding;
using NtfsCompare::Listing;
using NtfsCompare::PrintReport;
using NtfsCompare::Report;

namespace {

// A regular file of the given size, with every compared flag provided and
// clear.
Entry FileEntry(ULONGLONG size) {
  Entry entry;
  entry.logical_size = size;
  entry.read_only = false;
  entry.hidden = false;
  entry.system = false;
  entry.archive = false;
  entry.compressed = false;
  entry.encrypted = false;
  entry.sparse = false;
  return entry;
}

// A directory, with every compared flag provided and clear.
Entry DirectoryEntry() {
  Entry entry;
  entry.is_directory = true;
  entry.read_only = false;
  entry.hidden = false;
  entry.system = false;
  entry.archive = false;
  entry.compressed = false;
  entry.encrypted = false;
  entry.sparse = false;
  return entry;
}

}  // namespace

TEST_CASE("CompareAgainstReference reports nothing for equal listings",
          "[ntfscompare][compare][characterization]") {
  const Listing reference{{L"a.txt", FileEntry(5)}};
  const Listing candidate{{L"a.txt", FileEntry(5)}};
  Report report;

  CompareAgainstReference("method", reference, candidate, report);

  CHECK(report.findings.empty());
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().name == "method");
  CHECK(report.stats.front().compared_entries == 1);
  CHECK(report.stats.front().missing == 0);
  CHECK(report.stats.front().extra == 0);
  CHECK(report.stats.front().mismatched_fields == 0);
}

TEST_CASE("CompareAgainstReference reports a path the candidate lacks",
          "[ntfscompare][compare][characterization]") {
  const Listing reference{{L"a.txt", FileEntry(5)}};
  Report report;

  CompareAgainstReference("method", reference, Listing{}, report);

  REQUIRE(report.findings.size() == 1);
  const Finding& finding = report.findings.front();
  CHECK(finding.kind == "MISSING");
  CHECK(finding.method == "method");
  CHECK(finding.path == L"a.txt");
  CHECK(finding.field.empty());
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().missing == 1);
  CHECK(report.stats.front().compared_entries == 0);
}

TEST_CASE("CompareAgainstReference reports a path only the candidate has",
          "[ntfscompare][compare][characterization]") {
  const Listing candidate{{L"b.txt", FileEntry(3)}};
  Report report;

  CompareAgainstReference("method", Listing{}, candidate, report);

  REQUIRE(report.findings.size() == 1);
  const Finding& finding = report.findings.front();
  CHECK(finding.kind == "EXTRA");
  CHECK(finding.path == L"b.txt");
  CHECK(finding.field.empty());
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().extra == 1);
  CHECK(report.stats.front().compared_entries == 0);
}

TEST_CASE("CompareAgainstReference reports a size that differs",
          "[ntfscompare][compare][characterization]") {
  const Listing reference{{L"a.txt", FileEntry(5)}};
  const Listing candidate{{L"a.txt", FileEntry(7)}};
  Report report;

  CompareAgainstReference("method", reference, candidate, report);

  REQUIRE(report.findings.size() == 1);
  const Finding& finding = report.findings.front();
  CHECK(finding.kind == "MISMATCH");
  CHECK(finding.path == L"a.txt");
  CHECK(finding.field == "LogicalSize");
  CHECK(finding.expected == "5");
  CHECK(finding.actual == "7");
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().compared_entries == 1);
  CHECK(report.stats.front().mismatched_fields == 1);
}

TEST_CASE(
    "CompareAgainstReference reports a file the candidate calls a "
    "directory",
    "[ntfscompare][compare][characterization]") {
  const Listing reference{{L"a", FileEntry(5)}};
  const Listing candidate{{L"a", DirectoryEntry()}};
  Report report;

  CompareAgainstReference("method", reference, candidate, report);

  REQUIRE(report.findings.size() == 1);
  const Finding& finding = report.findings.front();
  CHECK(finding.kind == "MISMATCH");
  CHECK(finding.field == "Type");
  CHECK(finding.expected == "FILE");
  CHECK(finding.actual == "DIR");
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().mismatched_fields == 1);
}

TEST_CASE("CompareAgainstReference skips a field the candidate does not give",
          "[ntfscompare][compare][characterization]") {
  const Listing reference{{L"a.txt", FileEntry(5)}};
  Entry candidate_entry = FileEntry(7);
  candidate_entry.logical_size.reset();
  Report report;

  CompareAgainstReference("method", reference, {{L"a.txt", candidate_entry}},
                          report);

  CHECK(report.findings.empty());
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().compared_entries == 1);
  CHECK(report.stats.front().mismatched_fields == 0);
}

TEST_CASE("CompareAgainstReference writes a flag as true or false",
          "[ntfscompare][compare][characterization]") {
  Entry reference_entry = FileEntry(5);
  reference_entry.read_only = true;
  Entry candidate_entry = FileEntry(5);
  candidate_entry.read_only = false;
  Report report;

  CompareAgainstReference("method", {{L"a.txt", reference_entry}},
                          {{L"a.txt", candidate_entry}}, report);

  REQUIRE(report.findings.size() == 1);
  const Finding& finding = report.findings.front();
  CHECK(finding.field == "ReadOnly");
  CHECK(finding.expected == "true");
  CHECK(finding.actual == "false");
}

TEST_CASE("CompareAgainstReference reports each field that differs",
          "[ntfscompare][compare][characterization]") {
  Entry reference_entry = FileEntry(5);
  reference_entry.hidden = true;
  Entry candidate_entry = FileEntry(6);
  candidate_entry.hidden = false;
  Report report;

  CompareAgainstReference("method", {{L"a.txt", reference_entry}},
                          {{L"a.txt", candidate_entry}}, report);

  REQUIRE(report.findings.size() == 2);
  CHECK(report.findings[0].field == "LogicalSize");
  CHECK(report.findings[1].field == "Hidden");
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().mismatched_fields == 2);
}

TEST_CASE("CompareAgainstReference keeps one stats entry per method",
          "[ntfscompare][compare][characterization]") {
  const Listing reference{{L"a.txt", FileEntry(5)}};
  Report report;

  CompareAgainstReference("first", reference, reference, report);
  CompareAgainstReference("second", reference, Listing{}, report);

  REQUIRE(report.stats.size() == 2);
  CHECK(report.stats[0].name == "first");
  CHECK(report.stats[0].compared_entries == 1);
  CHECK(report.stats[1].name == "second");
  CHECK(report.stats[1].missing == 1);
}

TEST_CASE("CompareLibraryMethods reports an MftTree-only file as UNINDEXED",
          "[ntfscompare][compare][characterization]") {
  const Listing mft_tree{{L"link-a", FileEntry(5)}};
  Report report;

  const Listing reference =
      CompareLibraryMethods(Listing{}, Listing{}, mft_tree, report);

  REQUIRE(report.findings.size() == 1);
  const Finding& finding = report.findings.front();
  CHECK(finding.kind == "UNINDEXED");
  CHECK(finding.method == "mft-tree");
  CHECK(finding.path == L"link-a");
  CHECK(reference.contains(L"link-a"));
  CHECK(report.unindexed_paths == std::set<std::wstring>{L"link-a"});
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().compared_entries == 1);
  CHECK(report.stats.front().missing == 0);
  CHECK(report.stats.front().unindexed == 1);
}

TEST_CASE(
    "CompareLibraryMethods reports a file missing from one index as "
    "LIB-MISSING",
    "[ntfscompare][compare][characterization]") {
  const Listing full_cache{{L"a.txt", FileEntry(5)}};
  Report report;

  const Listing reference =
      CompareLibraryMethods(full_cache, Listing{}, Listing{}, report);

  REQUIRE(report.findings.size() == 2);
  CHECK(report.findings[0].kind == "LIB-MISSING");
  CHECK(report.findings[0].method == "no-cache");
  CHECK(report.findings[1].kind == "LIB-MISSING");
  CHECK(report.findings[1].method == "mft-tree");
  CHECK(report.unindexed_paths.empty());
  REQUIRE(report.stats.size() == 1);
  CHECK(report.stats.front().missing == 2);
  CHECK(report.stats.front().unindexed == 0);
  CHECK(reference.contains(L"a.txt"));
}

TEST_CASE(
    "CompareAgainstReference leaves UNINDEXED paths out of the OS "
    "comparison",
    "[ntfscompare][compare][characterization]") {
  const Listing indexed{{L"a.txt", FileEntry(5)}};
  const Listing mft_tree{{L"a.txt", FileEntry(5)}, {L"link-a", FileEntry(5)}};
  Report report;
  const Listing reference =
      CompareLibraryMethods(indexed, indexed, mft_tree, report);

  CompareAgainstReference("std::filesystem", reference, indexed, report);

  REQUIRE(report.findings.size() == 1);
  CHECK(report.findings.front().kind == "UNINDEXED");
  REQUIRE(report.stats.size() == 2);
  const auto& os_stats = report.stats.back();
  CHECK(os_stats.name == "std::filesystem");
  CHECK(os_stats.compared_entries == 1);
  CHECK(os_stats.missing == 0);
  CHECK(os_stats.extra == 0);
  CHECK(os_stats.unindexed == 1);
}

TEST_CASE("PrintReport does not fail on UNINDEXED alone",
          "[ntfscompare][compare][characterization]") {
  const Listing mft_tree{{L"link-a", FileEntry(5)}};
  Report report;
  const Listing reference =
      CompareLibraryMethods(Listing{}, Listing{}, mft_tree, report);

  CHECK(reference.contains(L"link-a"));
  CHECK_FALSE(PrintReport(report));
}
