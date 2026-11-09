#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/example/ntfsdir/arguments.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/types.h>

namespace ExampleNtfsDir = NTFS_BROWSER_NAMESPACE::Example::NtfsDir;
namespace Lib = NTFS_BROWSER_NAMESPACE;
using Catch::Matchers::ContainsSubstring;

namespace {

// What ParseArguments() made of a command line.
struct Parsed {
  std::optional<ExampleNtfsDir::Arguments> arguments;
  std::wstring error;
};

// Parses args, the command line without the program name.
Parsed Parse(const std::vector<std::wstring>& args) {
  std::vector<std::wstring> words{L"ntfsdir"};
  words.insert(words.end(), args.begin(), args.end());
  std::vector<wchar_t*> pointers;
  for (std::wstring& word : words) {
    pointers.push_back(word.data());
  }
  Parsed parsed;
  parsed.arguments = ExampleNtfsDir::ParseArguments(
      static_cast<int>(pointers.size()), pointers.data(), parsed.error);
  return parsed;
}

// The error text as ASCII, for a message of the test.
std::string Narrow(const std::wstring& text) {
  return std::filesystem::path(text).string();
}

// Checks that args cannot run and that the reason names needle.
void CheckRejected(const std::vector<std::wstring>& args,
                   const std::string& needle) {
  const Parsed parsed = Parse(args);
  CHECK_FALSE(parsed.arguments.has_value());
  CHECK_THAT(Narrow(parsed.error), ContainsSubstring(needle));
}

}  // namespace

TEST_CASE("NtfsDir takes a target and leaves every option at its default",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"disk.img"});

  REQUIRE(parsed.arguments.has_value());
  const Lib::Options& scan = parsed.arguments->scan;
  CHECK(scan.target == u"disk.img");
  CHECK(scan.engine == Lib::EngineKind::Auto);
  CHECK_FALSE(scan.recursive);
  CHECK_FALSE(scan.include_deleted);
  CHECK_FALSE(scan.recover_errors);
  CHECK_FALSE(scan.include_system_files);
  CHECK(scan.pattern.kind == Lib::PatternKind::None);
  CHECK_FALSE(scan.filter.size.IsSet());
  CHECK_FALSE(scan.filter.is_directory.has_value());
  CHECK_FALSE(parsed.arguments->progress);
}

TEST_CASE("NtfsDir reads its options before or after the target",
          "[io][ntfsdir][arguments]") {
  for (const auto& args :
       {std::vector<std::wstring>{L"-r", L"--deleted", L"disk.img"},
        std::vector<std::wstring>{L"disk.img", L"--recursive", L"--deleted"}}) {
    const Parsed parsed = Parse(args);

    REQUIRE(parsed.arguments.has_value());
    CHECK(parsed.arguments->scan.target == u"disk.img");
    CHECK(parsed.arguments->scan.recursive);
    CHECK(parsed.arguments->scan.include_deleted);
  }
}

TEST_CASE("NtfsDir copies --deleted and --recover to the scan",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"--deleted", L"--recover", L"disk.img"});

  REQUIRE(parsed.arguments.has_value());
  CHECK(parsed.arguments->scan.include_deleted);
  CHECK(parsed.arguments->scan.recover_errors);
}

TEST_CASE("NtfsDir names its engine with --engine",
          "[io][ntfsdir][arguments]") {
  const std::vector<std::pair<std::wstring, Lib::EngineKind>> engines{
      {L"auto", Lib::EngineKind::Auto},
      {L"mftlist", Lib::EngineKind::MftList},
      {L"nocache", Lib::EngineKind::DirectoryNoCache},
      {L"fullcache", Lib::EngineKind::DirectoryFullCache}};
  for (const auto& [name, kind] : engines) {
    INFO(Narrow(name));
    const Parsed parsed = Parse({L"--engine=" + name, L"disk.img"});

    REQUIRE(parsed.arguments.has_value());
    CHECK(parsed.arguments->scan.engine == kind);
  }
}

TEST_CASE("NtfsDir refuses an engine it does not know",
          "[io][ntfsdir][arguments]") {
  CheckRejected({L"--engine=turbo", L"disk.img"}, "Unknown engine \"turbo\"");
  CheckRejected({L"--engine=", L"disk.img"}, "Unknown engine");
  CheckRejected({L"--engine=MFTLIST", L"disk.img"}, "Unknown engine");
}

TEST_CASE("NtfsDir turns --name into a wildcard pattern",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"--name=*.txt", L"disk.img"});

  REQUIRE(parsed.arguments.has_value());
  const Lib::Pattern& pattern = parsed.arguments->scan.pattern;
  CHECK(pattern.kind == Lib::PatternKind::Wildcard);
  CHECK(pattern.text == u"*.txt");
  CHECK_FALSE(pattern.case_sensitive);
  CHECK(pattern.target == Lib::MatchTarget::Name);
}

TEST_CASE("NtfsDir keeps the units of a name that is past the BMP",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"--name=a\U0001F600?", L"disk.img"});

  REQUIRE(parsed.arguments.has_value());
  CHECK(parsed.arguments->scan.pattern.text == u"a\U0001F600?");
}

TEST_CASE("NtfsDir sets the target of a pattern and its case with flags",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed =
      Parse({L"--name=*", L"--match-path", L"--case", L"disk.img"});

  REQUIRE(parsed.arguments.has_value());
  CHECK(parsed.arguments->scan.pattern.target == Lib::MatchTarget::FullPath);
  CHECK(parsed.arguments->scan.pattern.case_sensitive);
}

TEST_CASE("NtfsDir refuses an empty pattern", "[io][ntfsdir][arguments]") {
  CheckRejected({L"--name=", L"disk.img"}, "--name needs a pattern");
}

TEST_CASE("NtfsDir turns --regex into a regular expression when it can",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"--regex=^a.*b$", L"disk.img"});

  if (Lib::RegexAvailable()) {
    REQUIRE(parsed.arguments.has_value());
    CHECK(parsed.arguments->scan.pattern.kind == Lib::PatternKind::Regex);
    CHECK(parsed.arguments->scan.pattern.text == u"^a.*b$");
  } else {
    CHECK_FALSE(parsed.arguments.has_value());
    CHECK_THAT(Narrow(parsed.error),
               ContainsSubstring("no regular expression support"));
  }
}

TEST_CASE("NtfsDir refuses --name and --regex together",
          "[io][ntfsdir][arguments]") {
  if (!Lib::RegexAvailable()) {
    SKIP("this build has no regular expressions");
  }

  CheckRejected({L"--name=a*", L"--regex=a", L"disk.img"},
                "cannot be used together");
  CheckRejected({L"--regex=a", L"--name=a*", L"disk.img"},
                "cannot be used together");
  CheckRejected({L"--regex=", L"disk.img"}, "--regex needs a pattern");
}

TEST_CASE("NtfsDir sets the kind of entry with --dirs-only and --files-only",
          "[io][ntfsdir][arguments]") {
  const Parsed dirs = Parse({L"--dirs-only", L"disk.img"});
  const Parsed files = Parse({L"--files-only", L"disk.img"});
  const Parsed both = Parse({L"--files-only", L"--files-only", L"disk.img"});

  REQUIRE(dirs.arguments.has_value());
  REQUIRE(files.arguments.has_value());
  REQUIRE(both.arguments.has_value());
  CHECK(dirs.arguments->scan.filter.is_directory == true);
  CHECK(files.arguments->scan.filter.is_directory == false);
  CHECK(both.arguments->scan.filter.is_directory == false);
}

TEST_CASE("NtfsDir refuses --dirs-only with --files-only",
          "[io][ntfsdir][arguments]") {
  CheckRejected({L"--dirs-only", L"--files-only", L"disk.img"},
                "--dirs-only and --files-only cannot be used together");
  CheckRejected({L"--files-only", L"--dirs-only", L"disk.img"},
                "cannot be used together");
}

TEST_CASE("NtfsDir sets the bounds of the size", "[io][ntfsdir][arguments]") {
  const Parsed parsed =
      Parse({L"--min-size=10", L"--max-size=18446744073709551615", L"d"});

  REQUIRE(parsed.arguments.has_value());
  const auto& size = parsed.arguments->scan.filter.size;
  CHECK(size.min == 10);
  CHECK(size.max == UINT64_MAX);
}

TEST_CASE("NtfsDir accepts a size bound alone and equal bounds",
          "[io][ntfsdir][arguments]") {
  const Parsed minimum = Parse({L"--min-size=0", L"disk.img"});
  const Parsed maximum = Parse({L"--max-size=7", L"disk.img"});
  const Parsed equal = Parse({L"--min-size=7", L"--max-size=7", L"disk.img"});

  REQUIRE(minimum.arguments.has_value());
  REQUIRE(maximum.arguments.has_value());
  REQUIRE(equal.arguments.has_value());
  CHECK(minimum.arguments->scan.filter.size.min == 0);
  CHECK_FALSE(minimum.arguments->scan.filter.size.max.has_value());
  CHECK_FALSE(maximum.arguments->scan.filter.size.min.has_value());
  CHECK(maximum.arguments->scan.filter.size.max == 7);
}

TEST_CASE("NtfsDir refuses a size that is not a number",
          "[io][ntfsdir][arguments]") {
  for (const std::wstring bad :
       {L"", L"abc", L"-5", L"+5", L"1.5", L"12k", L" 7",
        L"18446744073709551616", L"99999999999999999999999"}) {
    INFO("value \"" << Narrow(bad) << "\"");
    CheckRejected({L"--min-size=" + bad, L"disk.img"}, "--min-size needs a");
    CheckRejected({L"--max-size=" + bad, L"disk.img"}, "--max-size needs a");
  }
}

TEST_CASE("NtfsDir refuses a minimum size above the maximum",
          "[io][ntfsdir][arguments]") {
  CheckRejected({L"--min-size=9", L"--max-size=8", L"disk.img"},
                "--min-size is above --max-size");
}

TEST_CASE("NtfsDir sets system files and progress with flags",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"--system", L"--progress", L"disk.img"});

  REQUIRE(parsed.arguments.has_value());
  CHECK(parsed.arguments->scan.include_system_files);
  CHECK(parsed.arguments->progress);
}

TEST_CASE("NtfsDir keeps the log option of the other tools",
          "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"--log=console:off", L"disk.img"});

  REQUIRE(parsed.arguments.has_value());
  CHECK(parsed.arguments->common.log_config.console_level ==
        NTFS_BROWSER_NAMESPACE::Log::Level::Off);
  CheckRejected({L"--log=bogus", L"disk.img"}, "Malformed option --log=bogus");
}

TEST_CASE("NtfsDir refuses a command line that cannot run",
          "[io][ntfsdir][arguments]") {
  CheckRejected({}, "A target is needed");
  CheckRejected({L"-r"}, "A target is needed");
  CheckRejected({L"one", L"two"}, "Only one target is allowed");
  CheckRejected({L"--turbo", L"disk.img"}, "Unknown option --turbo");
  CheckRejected({L"-x", L"disk.img"}, "Unknown option -x");
}

TEST_CASE("NtfsDir needs no target for --help", "[io][ntfsdir][arguments]") {
  const Parsed parsed = Parse({L"--help"});

  REQUIRE(parsed.arguments.has_value());
  CHECK(parsed.arguments->common.help);
}

TEST_CASE("The usage of NtfsDir names every option",
          "[io][ntfsdir][arguments]") {
  const std::string usage = ExampleNtfsDir::UsageText();

  for (const char* option :
       {"--recursive", "-r", "--engine=<auto|mftlist|nocache|fullcache>",
        "--name=PATTERN", "--regex=PATTERN", "--match-path", "--case",
        "--dirs-only", "--files-only", "--min-size=N", "--max-size=N",
        "--system", "--progress", "--deleted", "--recover",
        "--log=", "--help"}) {
    INFO(option);
    CHECK_THAT(usage, ContainsSubstring(option));
  }
  CHECK_THAT(usage, ContainsSubstring("\"Directory of\" header"));
  CHECK_THAT(usage, ContainsSubstring("full path on each line"));
  CHECK_THAT(usage, ContainsSubstring("exit code is 0"));
}
