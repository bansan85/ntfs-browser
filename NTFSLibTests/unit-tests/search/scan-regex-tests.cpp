#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "scan-support.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Fake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using TestSearch::Outcome;
using TestSearch::RunScan;
using TestSearch::TempImage;
using NameSet = std::vector<std::u16string>;

namespace {

// Options that scan the whole tree of an image with the given engine and look
// for the regular expression text.
Lib::Options RegexOptions(const TempImage& image, Lib::EngineKind engine,
                          std::u16string text) {
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.recursive = true;
  options.pattern.kind = Lib::PatternKind::Regex;
  options.pattern.text = std::move(text);
  return options;
}

// The names the scan gave, sorted.
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

// How many times needle is in haystack.
std::size_t Count(const std::string& haystack, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

const std::u16string cafe = u"Café.txt";
const std::u16string greek = u"Ελλάδα.txt";
const std::u16string cyrillic = u"Привет.txt";
const std::u16string pair_name = u"a\U0001D11Eb.txt";
const std::u16string lone_name =
    u"a\xD834"
    u"b.txt";

}  // namespace

TEST_CASE("A regular expression gives the same names with every engine",
          "[search][regex]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  const Outcome outcome = RunScan(RegexOptions(image, engine, u"\\.txt$"));

  CHECK(outcome.state == Lib::State::Finished);
  CHECK(NamesOf(outcome) ==
        Sorted({cafe, greek, cyrillic, u"report.txt", u"Copy of report.txt",
                pair_name, u"axb.txt", lone_name, u"deep.txt"}));
  CHECK(outcome.progress.entries_found == outcome.rows.size());
}

TEST_CASE("A regular expression folds case with the volume table",
          "[search][regex]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = RegexOptions(image, engine, u"^caf.\\.(txt|doc)$");

  SECTION("case off") {
    CHECK(NamesOf(RunScan(options)) == Sorted({cafe, u"CAFÉ.DOC"}));
  }

  SECTION("case on") {
    options.pattern.case_sensitive = true;

    CHECK(RunScan(options).rows.empty());
  }

  SECTION("case on with the exact case") {
    options.pattern.case_sensitive = true;
    options.pattern.text = u"^Café\\.txt$";

    CHECK(NamesOf(RunScan(options)) == NameSet{cafe});
  }

  SECTION("Greek capitals find the lowercase name") {
    options.pattern.text = u"^ΕΛ";

    CHECK(NamesOf(RunScan(options)) == Sorted({greek, u"ΕΛΛΆΔΑ.DOC"}));
  }
}

TEST_CASE("A regular expression treats surrogates as characters or as holes",
          "[search][regex]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  SECTION("a dot takes a pair and skips an unpaired surrogate") {
    const Outcome outcome =
        RunScan(RegexOptions(image, engine, u"^a.b\\.txt$"));

    CHECK(NamesOf(outcome) == Sorted({pair_name, u"axb.txt"}));
  }

  SECTION("the name with the unpaired surrogate is still found by its ends") {
    const Outcome outcome = RunScan(RegexOptions(image, engine, u"^a"));

    CHECK(NamesOf(outcome) == Sorted({pair_name, u"axb.txt", lone_name}));
  }
}

TEST_CASE("A regular expression looks at the name or at the full path",
          "[search][regex]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = RegexOptions(image, engine, u"");

  SECTION("a name does not see the folder") {
    options.pattern.text = u"^docs$";

    CHECK(NamesOf(RunScan(options)) == NameSet{u"Docs"});
  }

  SECTION("a path sees the folders") {
    options.pattern.text = u"^\\\\docs\\\\[^\\\\]+\\.txt$";
    options.pattern.target = Lib::MatchTarget::FullPath;

    const Outcome outcome = RunScan(options);

    CHECK(NamesOf(outcome) == Sorted({cafe, greek, cyrillic, u"report.txt"}));
    for (const TestSearch::Row& row : outcome.rows) {
      CHECK_FALSE(row.path.empty());
    }
  }

  SECTION("a path reaches a file below two folders") {
    options.pattern.text = u"\\\\Deep\\\\Deeper\\\\";
    options.pattern.target = Lib::MatchTarget::FullPath;

    CHECK(NamesOf(RunScan(options)) == NameSet{u"deep.txt"});
  }
}

TEST_CASE("A regular expression finds a deleted file only when asked",
          "[search][regex]") {
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = RegexOptions(image, engine, u"^GONE-É\\.TXT$");

  SECTION("by default") { CHECK(RunScan(options).rows.empty()); }

  SECTION("with include_deleted") {
    options.include_deleted = true;

    const Outcome outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().name == u"gone-é.txt");
    CHECK(outcome.rows.front().standing != Lib::Standing::Live);
  }
}

TEST_CASE("A regular expression that does not compile fails the scan",
          "[search][regex]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::NamesImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);

  const Outcome outcome = RunScan(RegexOptions(image, engine, u"a(b"));

  CHECK(outcome.state == Lib::State::Failed);
  CHECK(outcome.rows.empty());
  CHECK(outcome.last_batches == 1);
  const std::string log = TestSupport::TakeCapturedLog();
  CHECK(log.find("Invalid regular expression") != std::string::npos);
  CHECK(log.find("(at unit 3)") != std::string::npos);
}

TEST_CASE("A runaway regular expression warns once for the whole scan",
          "[search][regex]") {
  TestSupport::InstallCaptureSink();
  std::vector<Fake::FakeTreeNode> nodes;
  for (const char16_t suffix : {u'1', u'2', u'3'}) {
    nodes.push_back({.links = {{.name = std::wstring(200, L'a') + L"b" +
                                        static_cast<wchar_t>(suffix)}},
                     .data_size = 1});
  }
  nodes.push_back({.links = {{.name = L"aaa"}}, .data_size = 1});
  const TempImage image(Fake::BuildFakeNtfsImageWithNameTree(nodes));
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  const auto start = std::chrono::steady_clock::now();

  const Outcome outcome = RunScan(RegexOptions(image, engine, u"^(a+)+$"));

  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
  CHECK(outcome.state == Lib::State::Finished);
  CHECK(NamesOf(outcome) == NameSet{u"aaa"});
  CHECK(Count(TestSupport::TakeCapturedLog(), "gave up") == 1);
}
