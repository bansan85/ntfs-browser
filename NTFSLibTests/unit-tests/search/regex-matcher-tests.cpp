#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "search/matcher.h"
#include "upcase/upcase.h"

namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using NTFS_BROWSER_NAMESPACE::UpCase::Table;

namespace {

// What a test asks of a matcher: the case rule and the target.
struct Query {
  bool case_sensitive{false};
  Lib::MatchTarget target{Lib::MatchTarget::Name};
};

// The regular expression matcher of text, or null.
std::unique_ptr<Lib::Matcher> Make(const Query& query,
                                   std::u16string_view text) {
  Lib::Pattern pattern;
  pattern.kind = Lib::PatternKind::Regex;
  pattern.text = std::u16string(text);
  pattern.case_sensitive = query.case_sensitive;
  pattern.target = query.target;
  return Lib::MakeMatcher(pattern, Table::BuiltIn());
}

// Whether the expression text matches name, and path for the full path target.
bool Match(const Query& query, std::u16string_view text,
           std::u16string_view name, std::u16string_view path = {}) {
  const std::unique_ptr<Lib::Matcher> matcher = Make(query, text);
  REQUIRE(matcher != nullptr);
  return matcher->Matches(name, path);
}

bool Rx(std::u16string_view text, std::u16string_view name) {
  return Match({}, text, name);
}

bool RxCase(std::u16string_view text, std::u16string_view name) {
  return Match({.case_sensitive = true}, text, name);
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

// A name that sends a backtracking engine into a very long run on (a+)+$.
std::u16string RunawayName() { return std::u16string(250, u'a') + u"b"; }

}  // namespace

TEST_CASE("The build says it has regular expressions", "[search][regex]") {
  CHECK(Lib::RegexAvailable());
}

TEST_CASE("A regular expression is a search in the name", "[search][regex]") {
  SECTION("a plain text is found anywhere") {
    CHECK(Rx(u"port", u"report.txt"));
    CHECK(Rx(u"report", u"report.txt"));
    CHECK(Rx(u"txt", u"report.txt"));
    CHECK_FALSE(Rx(u"reports", u"report.txt"));
  }

  SECTION("anchors hold it to the ends") {
    CHECK(Rx(u"^report", u"report.txt"));
    CHECK_FALSE(Rx(u"^port", u"report.txt"));
    CHECK(Rx(u"\\.txt$", u"report.txt"));
    CHECK_FALSE(Rx(u"\\.tx$", u"report.txt"));
    CHECK(Rx(u"^report\\.txt$", u"report.txt"));
    CHECK_FALSE(Rx(u"^report$", u"report.txt"));
  }

  SECTION("alternation") {
    CHECK(Rx(u"\\.(txt|doc)$", u"a.doc"));
    CHECK(Rx(u"\\.(txt|doc)$", u"a.txt"));
    CHECK_FALSE(Rx(u"\\.(txt|doc)$", u"a.pdf"));
    CHECK(Rx(u"cat|dog", u"hotdog"));
  }

  SECTION("classes and repeats") {
    CHECK(Rx(u"^[a-c]+[0-9]{2}\\.log$", u"abc42.log"));
    CHECK_FALSE(Rx(u"^[a-c]+[0-9]{2}\\.log$", u"abd42.log"));
    CHECK_FALSE(Rx(u"^[a-c]+[0-9]{2}\\.log$", u"abc4.log"));
    CHECK(Rx(u"^[^.]+$", u"README"));
    CHECK_FALSE(Rx(u"^[^.]+$", u"a.b"));
  }

  SECTION("escapes follow Unicode") {
    CHECK(Rx(u"^\\w+$", u"café"));
    CHECK(Rx(u"^\\w+$", u"Ελλάδα"));
    CHECK_FALSE(Rx(u"^\\w+$", u"a b"));
    CHECK(Rx(u"\\bfoo\\b", u"a foo b"));
    CHECK_FALSE(Rx(u"\\bfoo\\b", u"afoob"));
  }

  SECTION("a text that matches the empty string matches every name") {
    CHECK(Rx(u"x*", u"report.txt"));
  }

  SECTION("an empty text matches no name") {
    CHECK_FALSE(Rx(u"", u"report.txt"));
    CHECK_FALSE(Rx(u"", u""));
  }
}

TEST_CASE("A regular expression folds case through the table",
          "[search][regex]") {
  SECTION("ASCII") {
    CHECK(Rx(u"REPORT", u"report.txt"));
    CHECK(Rx(u"report", u"REPORT.TXT"));
    CHECK_FALSE(RxCase(u"REPORT", u"report.txt"));
    CHECK(RxCase(u"report", u"report.txt"));
  }

  SECTION("Latin with an accent") {
    CHECK(Rx(u"^cafÉ$", u"café"));
    CHECK(Rx(u"^café$", u"CAFÉ"));
    CHECK_FALSE(RxCase(u"^cafÉ$", u"café"));
    CHECK(RxCase(u"^café$", u"café"));
  }

  SECTION("Greek") {
    CHECK(Rx(u"^ΕΛΛΆΔΑ$", u"Ελλάδα"));
    CHECK_FALSE(RxCase(u"^ΕΛΛΆΔΑ$", u"Ελλάδα"));
  }

  SECTION("Cyrillic") {
    CHECK(Rx(u"^ПРИВЕТ$", u"привет"));
    CHECK_FALSE(RxCase(u"^ПРИВЕТ$", u"привет"));
  }

  SECTION("a letter that is no letter of another script") {
    CHECK_FALSE(Rx(u"^a$", u"а"));
  }

  SECTION("a class keeps its case rule") {
    CHECK(Rx(u"^[a-z]+$", u"ABC"));
    CHECK_FALSE(RxCase(u"^[a-z]+$", u"ABC"));
  }
}

TEST_CASE("A regular expression sees surrogates as the engine does",
          "[search][regex]") {
  SECTION("a pair is one character") {
    CHECK(Rx(u"^.$", u"\U0001D11E"));
    CHECK_FALSE(Rx(u"^..$", u"\U0001D11E"));
    CHECK(Rx(u"^a.b$", u"a\U0001D11Eb"));
    CHECK(Rx(u"\\x{1D11E}", u"a\U0001D11Eb"));
    CHECK(Rx(u"\U0001D11E", u"a\U0001D11Eb"));
    CHECK_FALSE(Rx(u"\U0001D11F", u"a\U0001D11Eb"));
  }

  SECTION("an unpaired surrogate matches nothing, not even a dot") {
    constexpr std::u16string_view lone =
        u"a\xD834"
        u"b";

    CHECK_FALSE(Rx(u"a.b", lone));
    CHECK_FALSE(Rx(u"^a[^x]b$", lone));
    CHECK(Rx(u"^a", lone));
    CHECK(Rx(u"b$", lone));
    CHECK(Rx(u"b", lone));
  }

  SECTION("a pair of another script is compared as it is") {
    CHECK(Rx(u"^a\U0001D11E$", u"A\U0001D11E"));
  }
}

TEST_CASE(
    "The full path target looks at the path only for a regular expression",
    "[search][regex]") {
  const Query path_query{.target = Lib::MatchTarget::FullPath};

  SECTION("anchors count from the start of the path") {
    CHECK(Match(path_query, u"^\\\\Docs\\\\", u"a.txt", u"\\Docs\\a.txt"));
    CHECK_FALSE(Match(path_query, u"^\\\\Other", u"a.txt", u"\\Docs\\a.txt"));
    CHECK(Match(path_query, u"\\\\Docs\\\\[^\\\\]+\\.txt$", u"a.txt",
                u"\\Docs\\a.txt"));
    CHECK_FALSE(Match(path_query, u"\\\\Docs\\\\[^\\\\]+\\.txt$", u"a.txt",
                      u"\\Docs\\Sub\\a.txt"));
  }

  SECTION("the name alone does not see the folders") {
    CHECK_FALSE(Rx(u"^\\\\Docs", u"a.txt"));
    CHECK(Rx(u"^a\\.txt$", u"a.txt"));
    CHECK_FALSE(Match(path_query, u"^a\\.txt$", u"a.txt", u"\\Docs\\a.txt"));
  }

  SECTION("case") {
    CHECK(Match(path_query, u"^\\\\DOCS\\\\A\\.TXT$", u"a.txt",
                u"\\Docs\\a.txt"));
    CHECK_FALSE(
        Match({.case_sensitive = true, .target = Lib::MatchTarget::FullPath},
              u"^\\\\DOCS\\\\A\\.TXT$", u"a.txt", u"\\Docs\\a.txt"));
  }
}

TEST_CASE("An expression that does not compile has no matcher",
          "[search][regex]") {
  TestSupport::InstallCaptureSink();

  SECTION("an open group") {
    CHECK(Make({}, u"a(b") == nullptr);

    const std::string log = TestSupport::TakeCapturedLog();
    CHECK(log.find("Invalid regular expression") != std::string::npos);
    CHECK(log.find("(at unit 3)") != std::string::npos);
  }

  SECTION("a bad repeat") {
    CHECK(Make({}, u"*a") == nullptr);
    CHECK(TestSupport::TakeCapturedLog().find("Invalid regular expression") !=
          std::string::npos);
  }

  SECTION("a bad class") {
    CHECK(Make({}, u"[a-") == nullptr);
    CHECK(TestSupport::TakeCapturedLog().find("Invalid regular expression") !=
          std::string::npos);
  }

  SECTION("an unpaired surrogate in the text") {
    CHECK(Make({},
               u"a\xD834"
               u"b") == nullptr);
    CHECK(TestSupport::TakeCapturedLog().find("Invalid regular expression") !=
          std::string::npos);
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}

TEST_CASE("A runaway expression ends fast and warns once", "[search][regex]") {
  TestSupport::InstallCaptureSink();
  const std::unique_ptr<Lib::Matcher> matcher = Make({}, u"(a+)+$");
  REQUIRE(matcher != nullptr);
  const std::u16string name = RunawayName();
  const auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < 5; ++i) {
    CHECK_FALSE(matcher->Matches(name, {}));
  }

  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed < std::chrono::seconds(10));
  const std::string log = TestSupport::TakeCapturedLog();
  CHECK(Count(log, "gave up") == 1);
  CHECK(log.find("counts as no match") != std::string::npos);
}

TEST_CASE("The expression still works after it gave up", "[search][regex]") {
  TestSupport::InstallCaptureSink();
  const std::unique_ptr<Lib::Matcher> matcher = Make({}, u"(a+)+$");
  REQUIRE(matcher != nullptr);

  CHECK_FALSE(matcher->Matches(RunawayName(), {}));
  CHECK(matcher->Matches(u"aaa", {}));
  CHECK_FALSE(matcher->Matches(u"aab", {}));
  static_cast<void>(TestSupport::TakeCapturedLog());
}

TEST_CASE("The limits of the pattern are applied", "[search][regex]") {
  TestSupport::InstallCaptureSink();
  Lib::Pattern pattern;
  pattern.kind = Lib::PatternKind::Regex;
  pattern.text = u"(a+)+$";

  SECTION("a small match limit gives up on a short name") {
    pattern.regex_match_limit = 10;
    const std::unique_ptr<Lib::Matcher> matcher =
        Lib::MakeMatcher(pattern, Table::BuiltIn());
    REQUIRE(matcher != nullptr);

    CHECK_FALSE(matcher->Matches(std::u16string(30, u'a') + u"b", {}));
    CHECK(TestSupport::TakeCapturedLog().find("gave up") != std::string::npos);
  }

  SECTION("a limit of 0 is the default") {
    pattern.regex_match_limit = 0;
    pattern.regex_jit_stack_bytes = 0;
    const std::unique_ptr<Lib::Matcher> matcher =
        Lib::MakeMatcher(pattern, Table::BuiltIn());
    REQUIRE(matcher != nullptr);

    CHECK(matcher->Matches(u"aaa", {}));
    CHECK_FALSE(matcher->Matches(u"aab", {}));
    CHECK(TestSupport::TakeCapturedLog().find("gave up") == std::string::npos);
  }

  SECTION("a tiny JIT stack still matches a plain name") {
    pattern.text = u"^[a-z]+\\.txt$";
    pattern.regex_jit_stack_bytes = 1;
    const std::unique_ptr<Lib::Matcher> matcher =
        Lib::MakeMatcher(pattern, Table::BuiltIn());
    REQUIRE(matcher != nullptr);

    CHECK(matcher->Matches(u"report.txt", {}));
    CHECK_FALSE(matcher->Matches(u"report.doc", {}));
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}
