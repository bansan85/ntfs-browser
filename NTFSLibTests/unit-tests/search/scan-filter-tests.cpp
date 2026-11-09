#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "scan-support.h"
#include "search/engine.h"
#include "search/field-filter.h"
#include "search/target.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
namespace Attr = NTFS_BROWSER_NAMESPACE::Search::Attribute;
using TestSearch::FilterDay;
using TestSearch::Outcome;
using TestSearch::RunScan;
using TestSearch::TempImage;
using NameSet = std::vector<std::u16string>;

namespace {

// The names, sorted, as a test writes them.
NameSet Sorted(NameSet names) {
  std::ranges::sort(names);
  return names;
}

// The names the scan gave, sorted.
NameSet NamesOf(const Outcome& outcome) {
  NameSet names;
  for (const TestSearch::Row& row : outcome.rows) {
    names.push_back(row.name);
  }
  return Sorted(std::move(names));
}

// The day of FilterImage() as a Date.
Lib::Date Day(std::uint32_t day) { return Lib::Date{.ticks = FilterDay(day)}; }

// Options that scan the whole tree of an image with the given engine, deleted
// files included.
Lib::Options FilterOptions(const TempImage& image, Lib::EngineKind engine) {
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.recursive = true;
  options.include_deleted = true;
  return options;
}

// Scans the image of FilterImage() with filter and gives the sorted names.
NameSet Filtered(const TempImage& image, Lib::EngineKind engine,
                 const Lib::Filter& filter) {
  Lib::Options options = FilterOptions(image, engine);
  options.filter = filter;
  const Outcome outcome = RunScan(options);
  CHECK(outcome.state == Lib::State::Finished);
  return NamesOf(outcome);
}

// Every name of FilterImage() that a scan with the deleted files lists. A hard
// link has one name for each link.
const NameSet all_names =
    Sorted({u"Docs", u"small.txt", u"medium.log", u"big.dat", u"Sub",
            u"deep.txt", u"link.txt", u"link2.txt", u"gone.txt", u"temp.bin"});

// A filter and the names that it keeps in FilterImage(), as every engine lists
// them.
struct FilterCase {
  const char* label;
  Lib::Filter filter;
  NameSet expected;
};

// The cases that every engine MUST agree on. They use only fields that every
// engine gives.
std::vector<FilterCase> SharedCases() {
  std::vector<FilterCase> cases;
  const auto add = [&cases](const char* label, Lib::Filter filter,
                            NameSet expected) {
    cases.push_back({label, std::move(filter), Sorted(std::move(expected))});
  };

  add("nothing set", {}, all_names);
  add("size min", {.size = {.min = 100}},
      {u"medium.log", u"big.dat", u"deep.txt"});
  add("size max", {.size = {.max = 100}},
      {u"Docs", u"small.txt", u"medium.log", u"Sub", u"deep.txt", u"link.txt",
       u"link2.txt", u"gone.txt", u"temp.bin"});
  add("size between", {.size = {.min = 8, .max = 50}},
      {u"small.txt", u"link.txt", u"link2.txt", u"gone.txt"});
  add("size past the largest", {.size = {.min = 401}}, {});
  add("allocated size", {.allocated_size = {.min = 104, .max = 104}},
      {u"medium.log", u"deep.txt"});
  add("created between", {.created = {.min = Day(40), .max = Day(100)}},
      {u"medium.log", u"big.dat", u"gone.txt"});
  add("modified from", {.modified = {.min = Day(100)}},
      {u"medium.log", u"big.dat", u"Sub", u"deep.txt", u"link.txt",
       u"link2.txt"});
  add("mft changed up to", {.mft_changed = {.max = Day(40)}},
      {u"Docs", u"small.txt", u"gone.txt", u"temp.bin"});
  add("accessed between", {.accessed = {.min = Day(150), .max = Day(200)}},
      {u"medium.log", u"big.dat", u"Sub", u"deep.txt"});
  add("attributes all", {.attributes_all = Attr::archive},
      {u"small.txt", u"medium.log", u"deep.txt", u"gone.txt"});
  add("attributes all of two", {.attributes_all = Attr::archive | Attr::hidden},
      {u"medium.log"});
  add("attributes none", {.attributes_none = Attr::hidden},
      {u"Docs", u"small.txt", u"big.dat", u"deep.txt", u"link.txt",
       u"link2.txt", u"gone.txt", u"temp.bin"});
  add("attributes all and none",
      {.attributes_all = Attr::archive, .attributes_none = Attr::system},
      {u"small.txt", u"medium.log", u"gone.txt"});
  add("directory bit", {.attributes_all = Lib::Attribute::directory},
      {u"Docs", u"Sub"});
  add("folders", {.is_directory = true}, {u"Docs", u"Sub"});
  add("files", {.is_directory = false},
      {u"small.txt", u"medium.log", u"big.dat", u"deep.txt", u"link.txt",
       u"link2.txt", u"gone.txt", u"temp.bin"});
  add("only live", {.deleted = Lib::DeletedFilter::OnlyLive},
      {u"Docs", u"small.txt", u"medium.log", u"big.dat", u"Sub", u"deep.txt",
       u"link.txt", u"link2.txt", u"temp.bin"});
  add("only deleted", {.deleted = Lib::DeletedFilter::OnlyDeleted},
      {u"gone.txt"});
  add("standing live", {.standing_in = {Lib::Standing::Live}},
      {u"Docs", u"small.txt", u"medium.log", u"big.dat", u"Sub", u"deep.txt",
       u"link.txt", u"link2.txt", u"temp.bin"});
  add("record range", {.record = {.min = 18, .max = 20}},
      {u"medium.log", u"big.dat", u"Sub"});
  add("parent is Docs", {.parent_record = 16},
      {u"big.dat", u"Sub", u"link2.txt"});
  add("parent is the root", {.parent_record = 5},
      {u"Docs", u"small.txt", u"medium.log", u"link.txt", u"gone.txt",
       u"temp.bin"});
  add("size and attributes",
      {.size = {.min = 100}, .attributes_all = Attr::archive},
      {u"medium.log", u"deep.txt"});
  add("files, modified, live",
      {.modified = {.min = Day(100)},
       .is_directory = false,
       .deleted = Lib::DeletedFilter::OnlyLive},
      {u"medium.log", u"big.dat", u"deep.txt", u"link.txt", u"link2.txt"});
  add("root, without Attr::archive",
      {.attributes_none = Attr::archive, .parent_record = 5},
      {u"Docs", u"link.txt", u"temp.bin"});
  return cases;
}

// A summary with the given counts of clusters.
Lib::ClusterSummary Summary(
    std::initializer_list<std::pair<Lib::ClusterCondition, std::uint64_t>>
        counts) {
  Lib::ClusterSummary summary;
  for (const auto& [condition, count] : counts) {
    summary.clusters.at(static_cast<std::size_t>(condition)) = count;
  }
  return summary;
}

// The entry of a file with the given size, as a filter sees it.
Lib::Entry SizedEntry(std::uint64_t size) {
  Lib::Entry entry;
  entry.size = size;
  return entry;
}

// Whether a filter, built for engine, passes entry.
bool Passes(const Lib::Filter& filter, const Lib::Entry& entry,
            Lib::EngineKind engine = Lib::EngineKind::MftList) {
  Lib::Options options;
  options.include_deleted = true;
  options.filter = filter;
  const auto field_filter = Lib::FieldFilter::Make(options, engine);
  REQUIRE(field_filter.has_value());
  return field_filter->Matches(entry);
}

}  // namespace

TEST_CASE("Every filter keeps the same names with every engine",
          "[search][filter]") {
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  for (const FilterCase& test : SharedCases()) {
    INFO(test.label);
    CHECK(Filtered(image, engine, test.filter) == test.expected);
  }
}

TEST_CASE("An unset filter delivers everything", "[search][filter]") {
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryFullCache);

  const Outcome outcome = RunScan(FilterOptions(image, engine));

  CHECK(NamesOf(outcome) == all_names);
  CHECK(outcome.progress.entries_found == all_names.size());
}

TEST_CASE("The bounds of a range are inclusive", "[search][filter]") {
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryFullCache);
  Lib::Filter filter;

  SECTION("a size that equals both bounds") {
    filter.size = {.min = 100, .max = 100};

    CHECK(Filtered(image, engine, filter) ==
          Sorted({u"medium.log", u"deep.txt"}));
  }

  SECTION("a minimum one above the size") {
    filter.size = {.min = 400};

    CHECK(Filtered(image, engine, filter) == NameSet{u"big.dat"});
    filter.size = {.min = 401};
    CHECK(Filtered(image, engine, filter).empty());
  }

  SECTION("a range that leaves out both neighbours") {
    filter.size = {.min = 9, .max = 99};

    CHECK(Filtered(image, engine, filter) ==
          Sorted({u"link.txt", u"link2.txt", u"gone.txt"}));
  }

  SECTION("a time that equals both bounds") {
    filter.modified = {.min = Day(150), .max = Day(150)};

    CHECK(Filtered(image, engine, filter) == Sorted({u"big.dat", u"Sub"}));
  }

  SECTION("a record that equals the maximum") {
    filter.record = {.max = 16};

    CHECK(Filtered(image, engine, filter) == NameSet{u"Docs"});
  }
}

TEST_CASE("A folder that fails the filter is still entered",
          "[search][filter]") {
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = FilterOptions(image, engine);
  options.filter.size = {.min = 100};

  SECTION("the folders are not listed and the files below them are") {
    CHECK(NamesOf(RunScan(options)) ==
          Sorted({u"medium.log", u"big.dat", u"deep.txt"}));
  }

  SECTION("a scan of a folder") {
    options.target = image.Target(u"Docs");

    CHECK(NamesOf(RunScan(options)) == Sorted({u"big.dat", u"deep.txt"}));
  }

  SECTION("without recursion the folders below are not entered") {
    options.target = image.Target(u"Docs");
    options.recursive = false;

    CHECK(NamesOf(RunScan(options)) == NameSet{u"big.dat"});
  }
}

TEST_CASE("A filter and a wildcard pattern are joined with AND",
          "[search][filter]") {
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = FilterOptions(image, engine);
  options.pattern.kind = Lib::PatternKind::Wildcard;

  SECTION("on the name") {
    options.pattern.text = u"*.TXT";
    options.filter.size = {.min = 50};

    CHECK(NamesOf(RunScan(options)) ==
          Sorted({u"deep.txt", u"link.txt", u"link2.txt"}));
  }

  SECTION("on the full path") {
    options.pattern.text = u"\\Docs\\*";
    options.pattern.target = Lib::MatchTarget::FullPath;
    options.filter.is_directory = false;

    CHECK(NamesOf(RunScan(options)) ==
          Sorted({u"big.dat", u"link2.txt", u"deep.txt"}));
  }

  SECTION("that nothing satisfies") {
    options.pattern.text = u"*.dat";
    options.filter.size = {.max = 399};

    const Outcome outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Finished);
    CHECK(outcome.rows.empty());
    CHECK(outcome.last_batches == 1);
  }
}

TEST_CASE("A filter and a regular expression are joined with AND",
          "[search][filter][regex]") {
  if (!Lib::RegexAvailable()) {
    SKIP("this build has no regular expressions");
  }
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = FilterOptions(image, engine);
  options.pattern.kind = Lib::PatternKind::Regex;

  SECTION("on the name") {
    options.pattern.text = u"^(link|deep|small)";
    options.filter.size = {.max = 50};

    CHECK(NamesOf(RunScan(options)) ==
          Sorted({u"small.txt", u"link.txt", u"link2.txt"}));
  }

  SECTION("on the full path") {
    options.pattern.text = u"^\\\\Docs\\\\Sub\\\\";
    options.pattern.target = Lib::MatchTarget::FullPath;
    options.filter.attributes_all = Lib::Attribute::archive;

    CHECK(NamesOf(RunScan(options)) == NameSet{u"deep.txt"});
  }
}

TEST_CASE("The deleted tests need the deleted files to be listed",
          "[search][filter]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = FilterOptions(image, engine);

  SECTION("only live, with the deleted files listed") {
    options.filter.deleted = Lib::DeletedFilter::OnlyLive;

    CHECK(NamesOf(RunScan(options)).size() == all_names.size() - 1);
  }

  SECTION("only deleted, with them not listed, finds nothing and warns") {
    options.include_deleted = false;
    options.filter.deleted = Lib::DeletedFilter::OnlyDeleted;

    const Outcome outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Finished);
    CHECK(outcome.rows.empty());
    CHECK(TestSupport::TakeCapturedLog().find("nothing will match") !=
          std::string::npos);
  }

  SECTION("any, with them not listed, does not warn") {
    options.include_deleted = false;

    CHECK(NamesOf(RunScan(options)).size() == all_names.size() - 1);
    CHECK(TestSupport::TakeCapturedLog().find("nothing will match") ==
          std::string::npos);
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}

TEST_CASE("The directory engines report the standing of a deleted entry",
          "[search][filter]") {
  const TempImage image(TestSearch::FilterImage());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);

  SECTION("deleted or reused") {
    CHECK(Filtered(image, engine,
                   {.standing_in = {Lib::Standing::Deleted,
                                    Lib::Standing::Reused}}) ==
          NameSet{u"gone.txt"});
  }

  SECTION("unreadable") {
    CHECK(Filtered(image, engine, {.standing_in = {Lib::Standing::Unreadable}})
              .empty());
  }
}

TEST_CASE("A filter on the number of links needs the MftList engine",
          "[search][filter]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::FilterImage());
  Lib::Filter filter;
  filter.link_count = {.min = 2};

  SECTION("the MftList engine keeps the names of a record with two") {
    CHECK(Filtered(image, Lib::EngineKind::MftList, filter) ==
          Sorted({u"link.txt", u"link2.txt"}));
    filter.link_count = {.max = 1};
    CHECK(Filtered(image, Lib::EngineKind::MftList, filter).size() ==
          all_names.size() - 2);
  }

  SECTION("a directory engine fails the scan") {
    const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                                 Lib::EngineKind::DirectoryFullCache);
    Lib::Options options = FilterOptions(image, engine);
    options.filter = filter;

    const Outcome outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Failed);
    CHECK(outcome.rows.empty());
    CHECK(outcome.last_batches == 1);
    CHECK(TestSupport::TakeCapturedLog().find(
              "link_count, which only the MftList engine gives") !=
          std::string::npos);
  }

  SECTION("Auto picks the MftList engine, for a folder too") {
    Lib::Options options = FilterOptions(image, Lib::EngineKind::Auto);
    options.target = image.Target(u"Docs");
    options.filter = filter;
    Lib::TargetParts folder;
    folder.inside = {L"Docs"};

    CHECK(Lib::ResolveEngine(options, folder) == Lib::EngineKind::MftList);
    CHECK(NamesOf(RunScan(options)) == NameSet{u"link2.txt"});
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}

TEST_CASE("A filter on the rarer attribute bits needs a directory engine",
          "[search][filter]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::FilterImage());
  Lib::Filter filter;
  filter.attributes_all = Lib::Attribute::temporary;

  SECTION("the directory engines read the bits of the index entry") {
    const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                                 Lib::EngineKind::DirectoryFullCache);

    CHECK(Filtered(image, engine, filter) == NameSet{u"temp.bin"});
    filter.attributes_all = 0;
    filter.attributes_none = Lib::Attribute::reparse_point;
    CHECK(Filtered(image, engine, filter).size() == all_names.size() - 1);
  }

  SECTION("the MftList engine fails the scan, with a mask of either kind") {
    Lib::Options options = FilterOptions(image, Lib::EngineKind::MftList);
    options.filter = filter;

    CHECK(RunScan(options).state == Lib::State::Failed);
    options.filter = {.attributes_none = Lib::Attribute::offline};
    CHECK(RunScan(options).state == Lib::State::Failed);
    CHECK(TestSupport::TakeCapturedLog().find(
              "which the MftList engine does not give") != std::string::npos);
  }

  SECTION("the MftList engine still serves the bits it reads") {
    filter.attributes_all = Lib::Attribute::archive;

    CHECK(Filtered(image, Lib::EngineKind::MftList, filter).size() == 4);
  }

  SECTION("Auto picks a directory engine, for the whole volume too") {
    Lib::Options options = FilterOptions(image, Lib::EngineKind::Auto);
    options.filter = filter;

    CHECK(Lib::ResolveEngine(options, {}) ==
          Lib::EngineKind::DirectoryFullCache);
    CHECK(NamesOf(RunScan(options)) == NameSet{u"temp.bin"});
  }

  SECTION("the standing Reused needs a directory engine") {
    Lib::Options options = FilterOptions(image, Lib::EngineKind::MftList);
    options.filter = {.standing_in = {Lib::Standing::Reused}};

    CHECK(RunScan(options).state == Lib::State::Failed);
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}

TEST_CASE("A filter that needs both kinds of engine fails the scan",
          "[search][filter]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::FilterImage());
  Lib::Options options = FilterOptions(image, Lib::EngineKind::Auto);
  options.filter.link_count = {.min = 1};
  options.filter.attributes_all = Lib::Attribute::temporary;

  const Outcome outcome = RunScan(options);

  CHECK(outcome.state == Lib::State::Failed);
  CHECK(outcome.rows.empty());
  CHECK(outcome.last_batches == 1);
  static_cast<void>(TestSupport::TakeCapturedLog());
}

TEST_CASE("A filter that can match nothing fails the scan",
          "[search][filter]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = FilterOptions(image, engine);
  std::string expected_log;

  SECTION("a size range with the minimum above the maximum") {
    options.filter.size = {.min = 10, .max = 9};
    expected_log = "filter on size";
  }

  SECTION("a time range with the minimum above the maximum") {
    options.filter.accessed = {.min = Day(2), .max = Day(1)};
    expected_log = "filter on accessed";
  }

  SECTION("an attribute bit that does not exist") {
    options.filter.attributes_none = 0x10000;
    expected_log = "a bit that does not exist";
  }

  SECTION("a cluster test with the maximum below the minimum") {
    options.filter.clusters = {{.condition = Lib::ClusterCondition::Intact,
                                .at_least = 3,
                                .at_most = 2}};
    expected_log = "filter on clusters";
  }

  SECTION("a cluster condition that does not exist") {
    options.engine = Lib::EngineKind::MftList;
    options.filter.clusters = {{.condition = static_cast<Lib::ClusterCondition>(
                                    Lib::cluster_condition_count)}};
    expected_log = "a condition that does not exist";
  }

  const Outcome outcome = RunScan(options);

  CHECK(outcome.state == Lib::State::Failed);
  CHECK(outcome.rows.empty());
  CHECK(outcome.last_batches == 1);
  CHECK(TestSupport::TakeCapturedLog().find(expected_log) != std::string::npos);
}

TEST_CASE("A scan that nothing satisfies ends with an empty last batch",
          "[search][filter]") {
  const TempImage image(TestSearch::FilterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = FilterOptions(image, engine);
  options.filter.size = {.min = 401};

  const Outcome outcome = RunScan(options);

  CHECK(outcome.state == Lib::State::Finished);
  CHECK(outcome.rows.empty());
  CHECK(outcome.batches >= 1);
  CHECK(outcome.last_batches == 1);
  CHECK(outcome.progress.entries_found == 0);
}

TEST_CASE("The filter on the data of deleted files", "[search][filter]") {
  const TempImage image(TestSearch::ReallocatedImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.include_deleted = true;

  SECTION("a state that the deleted files have") {
    options.filter.data_state_in = {Lib::DataState::Unknown};

    CHECK(NamesOf(RunScan(options)) == Sorted({u"old.bin", u"free.bin"}));
  }

  SECTION("a state that none of them has") {
    options.filter.data_state_in = {Lib::DataState::Intact};

    const Outcome outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Finished);
    CHECK(outcome.rows.empty());
    CHECK(outcome.last_batches == 1);
  }

  SECTION("the scan sets deleted_data_state when the filter needs it") {
    options.deleted_data_state = false;
    options.filter.data_state_in = {Lib::DataState::Unknown};

    CHECK(NamesOf(RunScan(options)) == Sorted({u"old.bin", u"free.bin"}));
  }

  SECTION("a data state of NotChecked lets the live files pass") {
    options.filter.data_state_in = {Lib::DataState::NotChecked};

    CHECK(NamesOf(RunScan(options)) == NameSet{u"live.bin"});
  }

  SECTION("two states") {
    options.filter.data_state_in = {Lib::DataState::NotChecked,
                                    Lib::DataState::Unknown};

    CHECK(NamesOf(RunScan(options)) ==
          Sorted({u"live.bin", u"old.bin", u"free.bin"}));
  }
}

TEST_CASE("The filter on the clusters of deleted files", "[search][filter]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::ReallocatedImage());
  Lib::Options options;
  options.target = image.Target();
  options.engine = Lib::EngineKind::MftList;
  options.include_deleted = true;
  using Lib::ClusterCondition;

  SECTION("at least one cluster that another file took") {
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_least = 1}};

    CHECK(NamesOf(RunScan(options)) == NameSet{u"old.bin"});
  }

  SECTION("at least as many as the file has") {
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_least = 2}};

    CHECK(NamesOf(RunScan(options)) == NameSet{u"old.bin"});
  }

  SECTION("more than the file has") {
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_least = 3}};

    const Outcome outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Finished);
    CHECK(outcome.rows.empty());
    CHECK(outcome.last_batches == 1);
  }

  SECTION("none that another file took: the live files have no summary") {
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_most = 0}};

    CHECK(NamesOf(RunScan(options)) == NameSet{u"free.bin"});
  }

  SECTION("every test has to pass") {
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_least = 1},
        {.condition = ClusterCondition::Intact, .at_least = 1}};

    CHECK(RunScan(options).rows.empty());
  }

  SECTION("the scan sets deleted_data_state when the filter needs it") {
    options.deleted_data_state = false;
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_least = 1}};

    CHECK(NamesOf(RunScan(options)) == NameSet{u"old.bin"});
  }

  SECTION("a directory engine fails the scan") {
    options.engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                              Lib::EngineKind::DirectoryFullCache);
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_least = 1}};

    const Outcome outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Failed);
    CHECK(outcome.last_batches == 1);
    CHECK(TestSupport::TakeCapturedLog().find(
              "clusters, which only the MftList engine gives") !=
          std::string::npos);
  }

  SECTION("Auto picks the MftList engine") {
    options.engine = Lib::EngineKind::Auto;
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_least = 1}};

    CHECK(Lib::ResolveEngine(options, {}) == Lib::EngineKind::MftList);
  }

  SECTION("the deleted files are not listed: nothing matches, and a warning") {
    options.include_deleted = false;
    options.filter.clusters = {
        {.condition = ClusterCondition::ReallocatedWritten, .at_most = 0}};

    CHECK(RunScan(options).rows.empty());
    CHECK(TestSupport::TakeCapturedLog().find("nothing will match") !=
          std::string::npos);
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}

TEST_CASE("A field filter tests every field that is set", "[search][filter]") {
  SECTION("an empty filter is not active and passes everything") {
    Lib::Options options;
    const auto filter =
        Lib::FieldFilter::Make(options, Lib::EngineKind::MftList);

    REQUIRE(filter.has_value());
    CHECK_FALSE(filter->IsActive());
    CHECK(filter->Matches(SizedEntry(0)));
  }

  SECTION("a range with one bound tests that side only") {
    Lib::Filter filter;
    filter.size = {.min = 10};

    CHECK_FALSE(Passes(filter, SizedEntry(9)));
    CHECK(Passes(filter, SizedEntry(10)));
    CHECK(Passes(filter, SizedEntry(UINT64_MAX)));
    filter.size = {.max = 10};
    CHECK(Passes(filter, SizedEntry(0)));
    CHECK(Passes(filter, SizedEntry(10)));
    CHECK_FALSE(Passes(filter, SizedEntry(11)));
  }

  SECTION("an entry with no time fails a range on that time") {
    Lib::Filter filter;
    filter.created = {.max = Day(5)};
    Lib::Entry entry;

    CHECK_FALSE(Passes(filter, entry));
    entry.created = Day(5);
    CHECK(Passes(filter, entry));
    entry.created = Day(6);
    CHECK_FALSE(Passes(filter, entry));
  }

  SECTION("the two attribute masks") {
    Lib::Filter filter;
    filter.attributes_all = Lib::Attribute::hidden | Lib::Attribute::system;
    filter.attributes_none = Lib::Attribute::read_only;
    Lib::Entry entry;

    entry.attributes = Lib::Attribute::hidden;
    CHECK_FALSE(Passes(filter, entry));
    entry.attributes = Lib::Attribute::hidden | Lib::Attribute::system;
    CHECK(Passes(filter, entry));
    entry.attributes |= Lib::Attribute::archive;
    CHECK(Passes(filter, entry));
    entry.attributes |= Lib::Attribute::read_only;
    CHECK_FALSE(Passes(filter, entry));
  }

  SECTION("live and deleted") {
    Lib::Entry entry;
    Lib::Filter live;
    live.deleted = Lib::DeletedFilter::OnlyLive;
    Lib::Filter deleted;
    deleted.deleted = Lib::DeletedFilter::OnlyDeleted;

    entry.standing = Lib::Standing::Live;
    CHECK(Passes(live, entry));
    CHECK_FALSE(Passes(deleted, entry));
    entry.standing = Lib::Standing::Deleted;
    CHECK_FALSE(Passes(live, entry));
    CHECK(Passes(deleted, entry));
    entry.standing = Lib::Standing::Reused;
    CHECK_FALSE(Passes(live, entry));
    CHECK(Passes(deleted, entry));
    entry.standing = Lib::Standing::Unreadable;
    CHECK_FALSE(Passes(live, entry));
    CHECK_FALSE(Passes(deleted, entry));
  }

  SECTION("a set of standings and a set of data states") {
    Lib::Filter filter;
    filter.standing_in = {Lib::Standing::Live, Lib::Standing::Unreadable};
    filter.data_state_in = {Lib::DataState::NotChecked};
    Lib::Entry entry;

    const auto engine = Lib::EngineKind::DirectoryNoCache;

    CHECK(Passes(filter, entry, engine));
    entry.standing = Lib::Standing::Reused;
    CHECK_FALSE(Passes(filter, entry, engine));
    entry.standing = Lib::Standing::Unreadable;
    CHECK(Passes(filter, entry, engine));
    entry.data_state = Lib::DataState::Bad;
    CHECK_FALSE(Passes(filter, entry, engine));
  }

  SECTION("a cluster test fails an entry with no summary") {
    Lib::Filter filter;
    filter.clusters = {{.condition = Lib::ClusterCondition::Intact}};
    Lib::Entry entry;

    CHECK_FALSE(Passes(filter, entry));
    const Lib::ClusterSummary summary =
        Summary({{Lib::ClusterCondition::Intact, 0}});
    entry.clusters = &summary;
    CHECK(Passes(filter, entry));
  }

  SECTION("a cluster test counts one condition") {
    Lib::Filter filter;
    filter.clusters = {
        {.condition = Lib::ClusterCondition::Bad, .at_least = 1, .at_most = 2}};
    Lib::Entry entry;
    Lib::ClusterSummary summary = Summary({{Lib::ClusterCondition::Bad, 0}});
    entry.clusters = &summary;

    CHECK_FALSE(Passes(filter, entry));
    summary = Summary({{Lib::ClusterCondition::Bad, 1}});
    CHECK(Passes(filter, entry));
    summary = Summary({{Lib::ClusterCondition::Bad, 2}});
    CHECK(Passes(filter, entry));
    summary = Summary(
        {{Lib::ClusterCondition::Bad, 3}, {Lib::ClusterCondition::Intact, 1}});
    CHECK_FALSE(Passes(filter, entry));
  }

  SECTION("the parent, the record and the links") {
    Lib::Filter filter;
    filter.parent_record = 5;
    filter.record = {.min = 16};
    filter.link_count = {.min = 1, .max = 2};
    Lib::Entry entry;
    entry.parent_record = 5;
    entry.record = 16;
    entry.link_count = 1;

    CHECK(Passes(filter, entry));
    entry.link_count = 3;
    CHECK_FALSE(Passes(filter, entry));
    entry.link_count = 2;
    entry.record = 15;
    CHECK_FALSE(Passes(filter, entry));
    entry.record = 16;
    entry.parent_record = 6;
    CHECK_FALSE(Passes(filter, entry));
  }
}

TEST_CASE("A filter tells what it asks of the scan", "[search][filter]") {
  SECTION("nothing") {
    const Lib::FilterNeeds needs = Lib::NeedsOf({});

    CHECK_FALSE(needs.mft_list_only);
    CHECK_FALSE(needs.directory_only);
    CHECK_FALSE(needs.deleted_data_state);
    CHECK_FALSE(needs.deleted_entries_only);
  }

  SECTION("the fields of one engine") {
    CHECK(Lib::NeedsOf({.link_count = {.min = 1}}).mft_list_only);
    CHECK(Lib::NeedsOf({.attributes_all = Lib::Attribute::offline})
              .directory_only);
    CHECK_FALSE(Lib::NeedsOf({.attributes_all = Lib::Attribute::hidden})
                    .directory_only);
    CHECK(
        Lib::NeedsOf({.standing_in = {Lib::Standing::Reused}}).directory_only);
  }

  SECTION("the data of deleted files") {
    const Lib::FilterNeeds states =
        Lib::NeedsOf({.data_state_in = {Lib::DataState::Intact}});
    const Lib::FilterNeeds not_checked =
        Lib::NeedsOf({.data_state_in = {Lib::DataState::NotChecked}});

    CHECK(states.deleted_data_state);
    CHECK(states.deleted_entries_only);
    CHECK_FALSE(not_checked.deleted_data_state);
    CHECK_FALSE(not_checked.deleted_entries_only);
    CHECK(
        Lib::NeedsOf({.clusters = {{.condition = Lib::ClusterCondition::Bad}}})
            .deleted_data_state);
  }

  SECTION("deleted entries only") {
    CHECK(Lib::NeedsOf({.deleted = Lib::DeletedFilter::OnlyDeleted})
              .deleted_entries_only);
    CHECK(Lib::NeedsOf({.standing_in = {Lib::Standing::Deleted}})
              .deleted_entries_only);
    CHECK_FALSE(Lib::NeedsOf({.standing_in = {Lib::Standing::Live,
                                              Lib::Standing::Deleted}})
                    .deleted_entries_only);
    CHECK_FALSE(Lib::NeedsOf({.deleted = Lib::DeletedFilter::OnlyLive})
                    .deleted_entries_only);
  }
}
