#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "scan-support.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using TestSearch::Outcome;
using TestSearch::Record;
using TestSearch::RunScan;
using TestSearch::TempImage;
using NameSet = std::vector<std::u16string>;

namespace {

// Options that scan the whole tree of an image with the given engine and look
// for text.
Lib::Options SearchOptions(const TempImage& image, Lib::EngineKind engine,
                           Lib::PatternKind kind, std::u16string text) {
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.recursive = true;
  options.pattern.kind = kind;
  options.pattern.text = std::move(text);
  return options;
}

// The names the scan gave, in order of name.
NameSet NamesOf(const Outcome& outcome) {
  NameSet names;
  for (const TestSearch::Row& row : outcome.rows) {
    names.push_back(row.name);
  }
  std::ranges::sort(names);
  return names;
}

// The names, sorted, as a test writes them.
NameSet Sorted(NameSet names) {
  std::ranges::sort(names);
  return names;
}

const std::u16string cafe = u"Café.txt";
const std::u16string greek = u"Ελλάδα.txt";
const std::u16string cyrillic = u"Привет.txt";
const std::u16string pair_name = u"a\U0001D11Eb.txt";
const std::u16string lone_name =
    u"a\xD834"
    u"b.txt";

}  // namespace

TEST_CASE("A wildcard search gives the same names with every engine",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  const auto outcome = RunScan(
      SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"*.txt"));

  CHECK(outcome.state == Lib::State::Finished);
  CHECK(NamesOf(outcome) ==
        Sorted({cafe, greek, cyrillic, u"report.txt", u"Copy of report.txt",
                pair_name, u"axb.txt", lone_name, u"deep.txt"}));
  CHECK(outcome.progress.entries_found == outcome.rows.size());
}

TEST_CASE("An exact search folds case through the volume table",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options =
      SearchOptions(image, engine, Lib::PatternKind::Exact, u"");

  SECTION("Latin") {
    options.pattern.text = u"CAFÉ.TXT";

    CHECK(NamesOf(RunScan(options)) == NameSet{cafe});
  }

  SECTION("Latin with case on") {
    options.pattern.text = u"CAFÉ.TXT";
    options.pattern.case_sensitive = true;

    const Outcome outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Finished);
    CHECK(outcome.rows.empty());
  }

  SECTION("Latin with the exact case") {
    options.pattern.text = cafe;
    options.pattern.case_sensitive = true;

    CHECK(NamesOf(RunScan(options)) == NameSet{cafe});
  }

  SECTION("Greek") {
    options.pattern.text = u"ΕΛΛΆΔΑ.TXT";

    CHECK(NamesOf(RunScan(options)) == NameSet{greek});
  }

  SECTION("Cyrillic") {
    options.pattern.text = u"привет.doc";

    CHECK(NamesOf(RunScan(options)) == NameSet{u"ПРИВЕТ.DOC"});
  }
}

TEST_CASE("A wildcard search with a question mark counts a pair once",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options =
      SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"");

  SECTION("one question mark") {
    options.pattern.text = u"a?b.txt";

    CHECK(NamesOf(RunScan(options)) ==
          Sorted({pair_name, u"axb.txt", lone_name}));
  }

  SECTION("two question marks") {
    options.pattern.text = u"a??b.txt";

    CHECK(RunScan(options).rows.empty());
  }
}

TEST_CASE("A wildcard search folds case with the volume table",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options =
      SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"caf*");

  SECTION("case off") {
    CHECK(NamesOf(RunScan(options)) == Sorted({cafe, u"CAFÉ.DOC"}));
  }

  SECTION("case on") {
    options.pattern.case_sensitive = true;

    CHECK(RunScan(options).rows.empty());
  }

  SECTION("Greek capitals find the lowercase name") {
    options.pattern.text = u"ΕΛ*";

    CHECK(NamesOf(RunScan(options)) == Sorted({greek, u"ΕΛΛΆΔΑ.DOC"}));
  }
}

TEST_CASE("A search looks at the name or at the full path",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options =
      SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"");

  SECTION("a name does not see the folder") {
    options.pattern.text = u"docs*";

    CHECK(NamesOf(RunScan(options)) == NameSet{u"Docs"});
  }

  SECTION("a path sees the folders") {
    options.pattern.text = u"\\docs\\*";
    options.pattern.target = Lib::MatchTarget::FullPath;

    const Outcome outcome = RunScan(options);

    CHECK(NamesOf(outcome) == Sorted({cafe, greek, cyrillic, u"report.txt"}));
    for (const TestSearch::Row& row : outcome.rows) {
      CHECK(row.path == u"\\Docs\\" + row.name);
    }
  }

  SECTION("the separator is matched by a star") {
    options.pattern.text = u"\\*\\deep.txt";
    options.pattern.target = Lib::MatchTarget::FullPath;

    const Outcome outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().path == u"\\Deep\\Deeper\\deep.txt");
  }

  SECTION("an exact path") {
    options.pattern.kind = Lib::PatternKind::Exact;
    options.pattern.text = u"\\DEEP\\DEEPER";
    options.pattern.target = Lib::MatchTarget::FullPath;

    const Outcome outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().is_directory);
    CHECK(outcome.rows.front().path == u"\\Deep\\Deeper");
  }

  SECTION("the path is built although the options did not ask") {
    options.full_path = false;
    options.pattern.text = u"*";
    options.pattern.target = Lib::MatchTarget::FullPath;

    const Outcome outcome = RunScan(options);

    REQUIRE_FALSE(outcome.rows.empty());
    for (const TestSearch::Row& row : outcome.rows) {
      CHECK_FALSE(row.path.empty());
    }
  }

  SECTION("a name search leaves the path alone") {
    options.pattern.text = u"*";

    const Outcome outcome = RunScan(options);

    REQUIRE_FALSE(outcome.rows.empty());
    for (const TestSearch::Row& row : outcome.rows) {
      CHECK(row.path.empty());
    }
  }
}

TEST_CASE("A search tests each name of a hard link on its own",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options =
      SearchOptions(image, engine, Lib::PatternKind::Exact, u"");

  SECTION("the name in the folder") {
    options.pattern.text = u"REPORT.TXT";

    const Outcome outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().record == Record(4));
    CHECK(outcome.rows.front().parent_record == Record(0));
  }

  SECTION("the name in the root") {
    options.pattern.text = u"copy of report.txt";

    const Outcome outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().name == u"Copy of report.txt");
    CHECK(outcome.rows.front().record == Record(4));
    CHECK(outcome.rows.front().parent_record == 5);
  }

  SECTION("both names with a wildcard") {
    options.pattern.kind = Lib::PatternKind::Wildcard;
    options.pattern.text = u"*report.txt";

    CHECK(RunScan(options).rows.size() == 2);
  }

  SECTION("both paths") {
    options.pattern.kind = Lib::PatternKind::Wildcard;
    options.pattern.text = u"*report.txt";
    options.pattern.target = Lib::MatchTarget::FullPath;

    const Outcome outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 2);
    CHECK(Sorted({outcome.rows.at(0).path, outcome.rows.at(1).path}) ==
          Sorted({u"\\Docs\\report.txt", u"\\Copy of report.txt"}));
  }
}

TEST_CASE("A search finds a deleted file only when asked",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options =
      SearchOptions(image, engine, Lib::PatternKind::Exact, u"GONE-É.TXT");

  SECTION("by default") { CHECK(RunScan(options).rows.empty()); }

  SECTION("with include_deleted") {
    options.include_deleted = true;

    const Outcome outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().name == u"gone-é.txt");
    CHECK(outcome.rows.front().standing == Lib::Standing::Deleted);
  }
}

TEST_CASE("A recursive search enters the folders that do not match",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  SECTION("the file below two folders") {
    const Outcome outcome = RunScan(
        SearchOptions(image, engine, Lib::PatternKind::Exact, u"deep.txt"));

    REQUIRE(outcome.rows.size() == 1);
    CHECK_FALSE(outcome.rows.front().is_directory);
    CHECK(outcome.rows.front().parent_record == Record(13));
  }

  SECTION("a folder that matches is listed, and the file in it is not") {
    const Outcome outcome = RunScan(
        SearchOptions(image, engine, Lib::PatternKind::Exact, u"deeper"));

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().is_directory);
  }

  SECTION("a folder that matches and a file below it") {
    const Outcome outcome = RunScan(
        SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"deep*"));

    CHECK(NamesOf(outcome) == Sorted({u"Deep", u"Deeper", u"deep.txt"}));
  }

  SECTION("without recursion the folder is not entered") {
    Lib::Options options =
        SearchOptions(image, engine, Lib::PatternKind::Exact, u"deep.txt");
    options.target = image.Target(u"Deep");
    options.recursive = false;

    CHECK(RunScan(options).rows.empty());
  }

  SECTION("from a folder") {
    Lib::Options options =
        SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"*.txt");
    options.target = image.Target(u"Docs");
    options.recursive = false;

    CHECK(NamesOf(RunScan(options)) ==
          Sorted({cafe, greek, cyrillic, u"report.txt"}));
  }
}

TEST_CASE("A search that matches nothing still ends with a last batch",
          "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  const Outcome outcome = RunScan(
      SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"*.nothing"));

  CHECK(outcome.state == Lib::State::Finished);
  CHECK(outcome.rows.empty());
  CHECK(outcome.last_batches == 1);
  CHECK(outcome.progress.entries_found == 0);
}

TEST_CASE("A search counts the delivered entries only", "[search][pattern]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options everything =
      SearchOptions(image, engine, Lib::PatternKind::None, u"");
  Lib::Options only_doc =
      SearchOptions(image, engine, Lib::PatternKind::Wildcard, u"*.doc");

  const Outcome all = RunScan(everything);
  const Outcome some = RunScan(only_doc);

  CHECK(all.progress.entries_found == all.rows.size());
  CHECK(some.rows.size() == 3);
  CHECK(some.progress.entries_found == 3);
  CHECK(all.rows.size() > some.rows.size());
}

TEST_CASE("A scan with a regular expression fails cleanly",
          "[search][pattern]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  const Outcome outcome =
      RunScan(SearchOptions(image, engine, Lib::PatternKind::Regex, u"a.*b"));

  CHECK(outcome.state == Lib::State::Failed);
  CHECK(outcome.rows.empty());
  CHECK(outcome.last_batches == 1);
  CHECK(TestSupport::TakeCapturedLog().find(
            "regex support is not compiled in") != std::string::npos);
}
