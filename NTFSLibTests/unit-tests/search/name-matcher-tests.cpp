#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "search/matcher.h"
#include "upcase/upcase.h"

namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using NTFS_BROWSER_NAMESPACE::UpCase::Table;

namespace {

// What a test asks of a matcher: the pattern, the case rule and the target.
struct Query {
  Lib::PatternKind kind{Lib::PatternKind::Wildcard};
  bool case_sensitive{false};
  Lib::MatchTarget target{Lib::MatchTarget::Name};
};

// Whether a matcher for text matches name, and path for the full path target.
bool Match(const Query& query, std::u16string_view text,
           std::u16string_view name, std::u16string_view path = {}) {
  Lib::Pattern pattern;
  pattern.kind = query.kind;
  pattern.text = std::u16string(text);
  pattern.case_sensitive = query.case_sensitive;
  pattern.target = query.target;
  const std::unique_ptr<Lib::Matcher> matcher =
      Lib::MakeMatcher(pattern, Table::BuiltIn());
  REQUIRE(matcher != nullptr);
  return matcher->Matches(name, path);
}

bool Exact(std::u16string_view text, std::u16string_view name) {
  return Match({.kind = Lib::PatternKind::Exact}, text, name);
}

bool ExactCase(std::u16string_view text, std::u16string_view name) {
  return Match({.kind = Lib::PatternKind::Exact, .case_sensitive = true}, text,
               name);
}

bool Wild(std::u16string_view text, std::u16string_view name) {
  return Match({}, text, name);
}

bool WildCase(std::u16string_view text, std::u16string_view name) {
  return Match({.case_sensitive = true}, text, name);
}

// A surrogate pair, U+1D11E.
constexpr std::u16string_view clef = u"\U0001D11E";

}  // namespace

TEST_CASE("An exact pattern needs the whole name", "[search][matcher]") {
  CHECK(Exact(u"report.txt", u"report.txt"));
  CHECK_FALSE(Exact(u"report", u"report.txt"));
  CHECK_FALSE(Exact(u"report.txt", u"report"));
  CHECK_FALSE(Exact(u"report.txt", u"report.txt2"));
  CHECK_FALSE(Exact(u"", u"report.txt"));
  CHECK_FALSE(Exact(u"", u""));

  SECTION("the wildcards are plain units") {
    CHECK(Exact(u"a*b", u"a*b"));
    CHECK_FALSE(Exact(u"a*b", u"axb"));
    CHECK(Exact(u"a?b", u"a?b"));
    CHECK_FALSE(Exact(u"a?b", u"axb"));
  }
}

TEST_CASE("An exact pattern folds case through the table",
          "[search][matcher]") {
  SECTION("ASCII") {
    CHECK(Exact(u"Report.TXT", u"rEPORT.txt"));
    CHECK_FALSE(ExactCase(u"Report.TXT", u"rEPORT.txt"));
    CHECK(ExactCase(u"Report.txt", u"Report.txt"));
  }

  SECTION("Latin with an accent") {
    CHECK(Exact(u"CafÉ", u"café"));
    CHECK_FALSE(ExactCase(u"CafÉ", u"café"));
  }

  SECTION("Greek") {
    CHECK(Exact(u"ΕΛΛΆΔΑ", u"Ελλάδα"));
    CHECK_FALSE(ExactCase(u"ΕΛΛΆΔΑ", u"Ελλάδα"));
  }

  SECTION("Cyrillic") {
    CHECK(Exact(u"ПРИВЕТ", u"привет"));
    CHECK_FALSE(ExactCase(u"ПРИВЕТ", u"привет"));
  }

  SECTION("a letter that is no letter of another script") {
    CHECK_FALSE(Exact(u"a", u"а"));
  }

  SECTION("a pair is compared as it is") {
    CHECK(Exact(u"a\U0001D11E", u"A\U0001D11E"));
    CHECK_FALSE(Exact(u"a\U0001D11E", u"a\U0001D11F"));
  }
}

TEST_CASE("A wildcard pattern has two wildcards", "[search][matcher]") {
  SECTION("star stands for any run, an empty one too") {
    CHECK(Wild(u"*", u"anything"));
    CHECK(Wild(u"*.txt", u"report.txt"));
    CHECK(Wild(u"*.txt", u".txt"));
    CHECK(Wild(u"report*", u"report"));
    CHECK(Wild(u"re*rt", u"report"));
    CHECK(Wild(u"a*b*c", u"axxbyyc"));
    CHECK(Wild(u"a**b", u"ab"));
    CHECK_FALSE(Wild(u"*.txt", u"report.txt.bak"));
    CHECK_FALSE(Wild(u"a*b*c", u"axxbyy"));
  }

  SECTION("question mark stands for one character") {
    CHECK(Wild(u"a?c", u"abc"));
    CHECK_FALSE(Wild(u"a?c", u"ac"));
    CHECK_FALSE(Wild(u"a?c", u"abbc"));
    CHECK(Wild(u"???", u"abc"));
    CHECK_FALSE(Wild(u"???", u"ab"));
    CHECK(Wild(u"*?", u"x"));
    CHECK_FALSE(Wild(u"*?", u""));
  }

  SECTION("the pattern covers the whole name") {
    CHECK_FALSE(Wild(u"doc", u"doc.txt"));
    CHECK_FALSE(Wild(u"oc*", u"doc.txt"));
    CHECK(Wild(u"doc*", u"doc.txt"));
    CHECK_FALSE(Wild(u"", u"doc"));
  }

  SECTION("a star is found again after a failed try") {
    CHECK(Wild(u"*ab", u"aab"));
    CHECK(Wild(u"*aab", u"aaab"));
    CHECK(Wild(u"a*a*b", u"aaaab"));
    CHECK_FALSE(Wild(u"a*a*b", u"aaaa"));
  }
}

TEST_CASE("A wildcard pattern folds case through the table",
          "[search][matcher]") {
  CHECK(Wild(u"CAF?.TXT", u"café.txt"));
  CHECK_FALSE(WildCase(u"CAF?.TXT", u"café.txt"));
  CHECK(WildCase(u"caf?.txt", u"café.txt"));
  CHECK(Wild(u"*ΆΔΑ", u"Ελλάδα"));
  CHECK(Wild(u"п*", u"Привет"));
  CHECK_FALSE(WildCase(u"п*", u"Привет"));
}

TEST_CASE("A question mark takes a surrogate pair as one character",
          "[search][matcher]") {
  const std::u16string pair = u"a" + std::u16string(clef) + u"b";
  const std::u16string lone =
      u"a\xD834"
      u"b";

  CHECK(Wild(u"a?b", pair));
  CHECK_FALSE(Wild(u"a??b", pair));
  CHECK(Wild(u"a?b", lone));
  CHECK(Wild(u"a*b", pair));
  CHECK(Wild(u"???", pair));
  CHECK(Wild(u"a?", u"a" + std::u16string(clef)));
  CHECK_FALSE(Wild(u"a?", u"a" + std::u16string(clef) + u"x"));
  CHECK(Wild(u"a" + std::u16string(clef) + u"b", pair));

  SECTION("a star never stops inside a pair") {
    CHECK_FALSE(Wild(u"*\xDD1E", pair));
    CHECK(Wild(u"*b", pair));
  }

  SECTION("two lone surrogates are two characters") {
    CHECK(Wild(u"??", u"\xD834\xD834"));
    CHECK_FALSE(Wild(u"?", u"\xD834\xD834"));
    CHECK(Wild(u"??", u"\xDD1E\xD834"));
  }
}

TEST_CASE("The full path target looks at the path only", "[search][matcher]") {
  const Query path_query{.target = Lib::MatchTarget::FullPath};
  const Query name_query{};

  SECTION("the separator is a unit like another") {
    CHECK(Match(path_query, u"\\Docs\\*", u"a.txt", u"\\Docs\\Sub\\a.txt"));
    CHECK(Match(path_query, u"\\*.txt", u"a.txt", u"\\Docs\\a.txt"));
    CHECK(Match(path_query, u"\\Docs?a.txt", u"a.txt", u"\\Docs\\a.txt"));
    CHECK_FALSE(Match(path_query, u"\\Docs\\*", u"a.txt", u"\\Other\\a.txt"));
  }

  SECTION("the name is not tested") {
    CHECK_FALSE(Match(path_query, u"a.txt", u"a.txt", u"\\Docs\\a.txt"));
    CHECK(Match(name_query, u"a.txt", u"a.txt", u"\\Docs\\a.txt"));
    CHECK_FALSE(
        Match(name_query, u"\\Docs\\a.txt", u"a.txt", u"\\Docs\\a.txt"));
  }

  SECTION("case") {
    CHECK(Match(path_query, u"\\DOCS\\A.TXT", u"a.txt", u"\\Docs\\a.txt"));
    CHECK_FALSE(
        Match({.case_sensitive = true, .target = Lib::MatchTarget::FullPath},
              u"\\DOCS\\A.TXT", u"a.txt", u"\\Docs\\a.txt"));
  }

  SECTION("exact") {
    const Query exact{.kind = Lib::PatternKind::Exact,
                      .target = Lib::MatchTarget::FullPath};
    CHECK(Match(exact, u"\\docs\\a.txt", u"a.txt", u"\\Docs\\a.txt"));
    CHECK_FALSE(Match(exact, u"\\docs", u"a.txt", u"\\Docs\\a.txt"));
  }
}

TEST_CASE("A pathological pattern ends quickly", "[search][matcher]") {
  const std::u16string run(1000000, u'a');
  const auto start = std::chrono::steady_clock::now();

  CHECK_FALSE(Wild(u"*a*a*a*a*a*b", run));
  CHECK_FALSE(Wild(u"a*a*a*a*a*a*a*a*a*c", run));
  CHECK_FALSE(Wild(u"*aaaaaaaaaa*aaaaaaaaaa*b", run));
  CHECK(Wild(u"*a*a*a*a*a*a", run));
  CHECK(Wild(u"*?*?*?*?*?*?*?*?*?*?", run));
  CHECK_FALSE(WildCase(u"*A*A*A*A*A*B", run));

  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed < std::chrono::seconds(10));
}

TEST_CASE("A pattern that cannot be used has no matcher", "[search][matcher]") {
  TestSupport::InstallCaptureSink();
  Lib::Pattern pattern;
  pattern.text = u"x";

#ifndef NTFS_BROWSER_ENABLE_REGEX
  SECTION("a regular expression without support") {
    pattern.kind = Lib::PatternKind::Regex;

    CHECK(Lib::MakeMatcher(pattern, Table::BuiltIn()) == nullptr);
    CHECK(TestSupport::TakeCapturedLog().find(
              "regex support is not compiled in") != std::string::npos);
  }
#endif

  SECTION("no pattern") {
    pattern.kind = Lib::PatternKind::None;

    CHECK(Lib::MakeMatcher(pattern, Table::BuiltIn()) == nullptr);
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}
