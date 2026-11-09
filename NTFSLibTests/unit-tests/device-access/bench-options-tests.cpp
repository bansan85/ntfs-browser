#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>

#include "bench/bench.h"
#include "tuning/tuning.h"
#include "volume-options.h"

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using Catch::Matchers::ContainsSubstring;
using ToolDeviceAccess::BenchOptions;
using ToolDeviceAccess::OptionResult;
using ToolDeviceAccess::ParseBenchOption;

namespace {

// The variable that makes the tools plan for an unknown device. The tests keep
// it set, for the tools they run as child processes.
constexpr const char* no_host_media_variable =
    "NTFS_BROWSER_NO_HOST_MEDIA_QUERY";

// Gives the variable a value for the scope (empty clears it), then sets it
// back as the tests keep it. It goes through the C runtime, which the tools
// read with std::getenv().
class NoHostMediaScope final {
 public:
  explicit NoHostMediaScope(const char* value) { Set(value); }

  NoHostMediaScope(const NoHostMediaScope&) = delete;
  NoHostMediaScope& operator=(const NoHostMediaScope&) = delete;
  NoHostMediaScope(NoHostMediaScope&&) = delete;
  NoHostMediaScope& operator=(NoHostMediaScope&&) = delete;

  ~NoHostMediaScope() { Set("1"); }

 private:
  static void Set(const char* value) {
#ifdef _WIN32
    _putenv_s(no_host_media_variable, value);
#else
    if (*value == '\0') {
      unsetenv(no_host_media_variable);
    } else {
      setenv(no_host_media_variable, value, 1);
    }
#endif
  }
};

// text with each unit cut to a char: the arguments of the tests are ASCII.
std::string Narrow(std::wstring_view text) {
  std::string narrow;
  for (const wchar_t unit : text) {
    narrow.push_back(static_cast<char>(unit));
  }
  return narrow;
}

// What ParseBenchOption() made of one argument.
struct Parsed {
  OptionResult result{OptionResult::NotMine};
  std::string error;
};

Parsed Parse(std::wstring_view arg, BenchOptions& options) {
  Parsed parsed;
  parsed.result = ParseBenchOption(arg, options, parsed.error);
  return parsed;
}

// The arguments in order, into one set of options.
BenchOptions ParseAll(std::initializer_list<std::wstring_view> args) {
  BenchOptions options;
  for (const std::wstring_view arg : args) {
    const Parsed parsed = Parse(arg, options);
    INFO("argument " << Narrow(arg));
    REQUIRE(parsed.result == OptionResult::Accepted);
  }
  return options;
}

// Why the argument is refused, which the test cannot go on without.
std::string MalformedBecause(std::wstring_view arg) {
  BenchOptions options;
  const Parsed parsed = Parse(arg, options);
  INFO("argument " << Narrow(arg));
  REQUIRE(parsed.result == OptionResult::Malformed);
  REQUIRE_FALSE(parsed.error.empty());
  return parsed.error;
}

}  // namespace

TEST_CASE("ParseBenchOption leaves an argument that is not its own",
          "[device-access][bench-options]") {
  for (const std::wstring_view arg : {L"--deleted",
                                      L"--recover",
                                      L"--recursive",
                                      L"--log=console:trace",
                                      L"--threads=2",
                                      L"c:",
                                      L"image.img",
                                      L"",
                                      L"--",
                                      L"-benchmark",
                                      L"--benchmarks",
                                      L"--benchmark2=x",
                                      L"--benchmark-x",
                                      L"--captured=x",
                                      L"--capture",
                                      L"--stop-after",
                                      L"--profile",
                                      L"--disk",
                                      L"--tune",
                                      L"--tuning=1",
                                      L"--tunes=a=1",
                                      L"--Benchmark",
                                      L"--TUNE=a=1"}) {
    BenchOptions options;
    INFO("argument " << Narrow(arg));
    const Parsed parsed = Parse(arg, options);
    CHECK(parsed.result == OptionResult::NotMine);
    CHECK(parsed.error.empty());
    CHECK_FALSE(options.WantsSession());
    CHECK(options.tunes.empty());
  }
}

TEST_CASE("ParseBenchOption reads --benchmark",
          "[device-access][bench-options]") {
  BenchOptions defaults;
  CHECK_FALSE(defaults.benchmark);
  CHECK(defaults.summary_path == std::filesystem::path("ntfs-benchmark.json"));
  CHECK_FALSE(defaults.WantsSession());

  const BenchOptions plain = ParseAll({L"--benchmark"});
  CHECK(plain.benchmark);
  CHECK(plain.WantsSession());
  CHECK(plain.summary_path == defaults.summary_path);

  const BenchOptions named = ParseAll({L"--benchmark=out/run 1.json"});
  CHECK(named.benchmark);
  CHECK(named.summary_path == std::filesystem::path("out/run 1.json"));

  // The path is everything after the first equals sign.
  const BenchOptions equals = ParseAll({L"--benchmark=a=b.json"});
  CHECK(equals.summary_path == std::filesystem::path("a=b.json"));

  const BenchOptions unicode = ParseAll({L"--benchmark=résumé.json"});
  CHECK(unicode.summary_path.filename() ==
        std::filesystem::path(L"résumé.json"));
}

TEST_CASE("ParseBenchOption refuses --benchmark= with no path",
          "[device-access][bench-options]") {
  CHECK_THAT(MalformedBecause(L"--benchmark="),
             ContainsSubstring("needs a path"));
}

TEST_CASE("ParseBenchOption reads --capture",
          "[device-access][bench-options]") {
  const BenchOptions options = ParseAll({L"--capture=image.sparse"});

  CHECK(options.capture_path == std::filesystem::path("image.sparse"));
  CHECK(options.WantsSession());
  CHECK_FALSE(options.benchmark);
  CHECK_THAT(MalformedBecause(L"--capture="),
             ContainsSubstring("needs a path"));
}

TEST_CASE("ParseBenchOption reads --stop-after",
          "[device-access][bench-options]") {
  using Bench::Phase;
  const std::pair<std::wstring_view, Phase> phases[] = {
      {L"open", Phase::Open},          {L"pass1", Phase::Pass1},
      {L"pass2", Phase::Pass2},        {L"flush", Phase::DataStateFlush},
      {L"sweep", Phase::ClusterSweep}, {L"check", Phase::ContentCheck},
      {L"link", Phase::Link},          {L"journal", Phase::JournalLoad},
      {L"walk", Phase::Walk},          {L"print", Phase::Print},
      {L"destroy", Phase::Destroy}};
  for (const auto& [name, phase] : phases) {
    INFO("phase " << Narrow(name));
    const BenchOptions options =
        ParseAll({std::wstring(L"--stop-after=").append(name)});
    REQUIRE(options.stop_after.has_value());
    CHECK(*options.stop_after == phase);
    // Stopping is not timing: it asks for no session.
    CHECK_FALSE(options.WantsSession());
  }

  for (const std::wstring_view bad :
       {L"--stop-after=", L"--stop-after=pass3", L"--stop-after=PASS1",
        L"--stop-after= pass1", L"--stop-after=pass1,pass2"}) {
    const std::string error = MalformedBecause(bad);
    CHECK_THAT(error, ContainsSubstring("--stop-after names the phase"));
    CHECK_THAT(error, ContainsSubstring("open, pass1, pass2, flush"));
  }
}

TEST_CASE("ParseBenchOption reads --profile and --disk",
          "[device-access][bench-options]") {
  const BenchOptions options =
      ParseAll({L"--profile=profiles/my disk.json", L"--disk=ACME Fast|SN 1"});

  CHECK(options.profile_path == std::filesystem::path("profiles/my disk.json"));
  CHECK(options.disk == "ACME Fast|SN 1");
  CHECK_THAT(MalformedBecause(L"--profile="),
             ContainsSubstring("needs a path"));

  // The last --disk wins; an empty one gives the disk back to the target.
  const BenchOptions again = ParseAll({L"--disk=a", L"--disk=b"});
  CHECK(again.disk == "b");
  CHECK(ParseAll({L"--disk=a", L"--disk="}).disk.empty());
}

TEST_CASE("ParseBenchOption reads --tune", "[device-access][bench-options]") {
  const BenchOptions options = ParseAll(
      {L"--tune=mft.window_bytes=65536", L"--tune=reader.max_reads@M2=8",
       L"--tune=probe.max_gap_bytes=0", L"--tune=a=b=7",
       L"--tune=x@M4=18446744073709551615"});

  REQUIRE(options.tunes.size() == 5);
  CHECK(options.tunes.at(0).name == "mft.window_bytes");
  CHECK_FALSE(options.tunes.at(0).pattern.has_value());
  CHECK(options.tunes.at(0).value == 65536);
  CHECK(options.tunes.at(1).name == "reader.max_reads");
  CHECK(options.tunes.at(1).pattern == Tuning::Pattern::ScatteredGrouped);
  CHECK(options.tunes.at(1).value == 8);
  CHECK(options.tunes.at(2).value == 0);
  // The value is what follows the last equals sign.
  CHECK(options.tunes.at(3).name == "a=b");
  CHECK(options.tunes.at(3).value == 7);
  CHECK(options.tunes.at(4).pattern == Tuning::Pattern::LargeData);
  CHECK(options.tunes.at(4).value == UINT64_MAX);
  CHECK_FALSE(options.WantsSession());
}

TEST_CASE("ParseBenchOption names the pattern of --tune",
          "[device-access][bench-options]") {
  const std::pair<std::wstring_view, Tuning::Pattern> patterns[] = {
      {L"M1", Tuning::Pattern::LargeSequential},
      {L"M2", Tuning::Pattern::ScatteredGrouped},
      {L"M3", Tuning::Pattern::DependentSmall},
      {L"M4", Tuning::Pattern::LargeData}};
  for (const auto& [name, pattern] : patterns) {
    const BenchOptions options = ParseAll(
        {std::wstring(L"--tune=reader.max_reads@").append(name).append(L"=3")});
    REQUIRE(options.tunes.size() == 1);
    CHECK(options.tunes.front().pattern == pattern);
  }
  for (const std::wstring_view bad :
       {L"--tune=a@M5=1", L"--tune=a@m1=1", L"--tune=a@=1", L"--tune=a@M=1",
        L"--tune=a@M12=1", L"--tune=a@ M1=1"}) {
    CHECK_THAT(MalformedBecause(bad),
               ContainsSubstring("it is M1, M2, M3 or M4"));
  }
}

TEST_CASE("ParseBenchOption refuses a malformed --tune",
          "[device-access][bench-options]") {
  CHECK_THAT(MalformedBecause(L"--tune=mft.window_bytes"),
             ContainsSubstring("--tune expects name=value"));
  CHECK_THAT(MalformedBecause(L"--tune="),
             ContainsSubstring("--tune expects name=value"));
  for (const std::wstring_view bad :
       {L"--tune=a=", L"--tune=a=x", L"--tune=a=-1", L"--tune=a=1.5",
        L"--tune=a=+1", L"--tune=a= 1", L"--tune=a=1 ", L"--tune=a=0x10",
        L"--tune=a=1e3", L"--tune=a=18446744073709551616",
        L"--tune=a=99999999999999999999999"}) {
    CHECK_THAT(MalformedBecause(bad),
               ContainsSubstring("a whole number after the equals sign"));
  }
}

TEST_CASE("ParseBenchOption reads --media", "[device-access][bench-options]") {
  CHECK_FALSE(BenchOptions{}.media.has_value());
  CHECK(ParseAll({L"--media=ssd"}).media == Tuning::MediaKind::SolidState);
  CHECK(ParseAll({L"--media=hdd"}).media == Tuning::MediaKind::Rotational);
  CHECK(ParseAll({L"--media=unknown"}).media == Tuning::MediaKind::Unknown);
  // The last one wins, as with the other options.
  CHECK(ParseAll({L"--media=ssd", L"--media=hdd"}).media ==
        Tuning::MediaKind::Rotational);
}

TEST_CASE("ParseBenchOption refuses a --media it does not know",
          "[device-access][bench-options]") {
  for (const std::wstring_view arg :
       {L"--media=", L"--media=floppy", L"--media=SSD", L"--media=nvme"}) {
    CHECK_THAT(MalformedBecause(arg), ContainsSubstring("ssd, hdd or unknown"));
  }
  // Without an equals sign it is not this option.
  BenchOptions options;
  CHECK(Parse(L"--media", options).result == OptionResult::NotMine);
}

TEST_CASE("ApplyTuning puts --media in the volume options",
          "[device-access][bench-options]") {
  const NoHostMediaScope cleared("");
  NTFS_BROWSER_NAMESPACE::VolumeOptions options;
  ToolDeviceAccess::AppliedTuning applied;

  REQUIRE(ToolDeviceAccess::ApplyTuning(BenchOptions{}, L"image.img", options,
                                        applied)
              .empty());
  CHECK_FALSE(options.media_kind.has_value());

  REQUIRE(ToolDeviceAccess::ApplyTuning(ParseAll({L"--media=hdd"}),
                                        L"image.img", options, applied)
              .empty());
  CHECK(options.media_kind == Tuning::MediaKind::Rotational);
}

TEST_CASE("The variable of the tests makes the tools plan for Unknown",
          "[device-access][bench-options]") {
  const NoHostMediaScope set("1");
  NTFS_BROWSER_NAMESPACE::VolumeOptions options;
  ToolDeviceAccess::AppliedTuning applied;

  REQUIRE(ToolDeviceAccess::ApplyTuning(BenchOptions{}, L"image.img", options,
                                        applied)
              .empty());
  CHECK(options.media_kind == Tuning::MediaKind::Unknown);

  REQUIRE(ToolDeviceAccess::ApplyTuning(ParseAll({L"--media=ssd"}),
                                        L"image.img", options, applied)
              .empty());
  CHECK(options.media_kind == Tuning::MediaKind::SolidState);
}

TEST_CASE("MediaKindName names each kind", "[device-access][bench-options]") {
  CHECK(ToolDeviceAccess::MediaKindName(Tuning::MediaKind::Unknown) ==
        "unknown");
  CHECK(ToolDeviceAccess::MediaKindName(Tuning::MediaKind::SolidState) ==
        "solid_state");
  CHECK(ToolDeviceAccess::MediaKindName(Tuning::MediaKind::Rotational) ==
        "rotational");
}

TEST_CASE("The options of a command line add up in order",
          "[device-access][bench-options]") {
  const BenchOptions options = ParseAll(
      {L"--benchmark=s.json", L"--capture=c.img", L"--stop-after=pass2",
       L"--profile=p.json", L"--disk=d", L"--tune=mft.window_bytes=4096",
       L"--tune=mft.window_bytes=8192"});

  CHECK(options.benchmark);
  CHECK(options.WantsSession());
  CHECK(options.summary_path == std::filesystem::path("s.json"));
  CHECK(options.capture_path == std::filesystem::path("c.img"));
  CHECK(options.stop_after == Bench::Phase::Pass2);
  CHECK(options.profile_path == std::filesystem::path("p.json"));
  CHECK(options.disk == "d");
  // Both --tune stay, in order: the later one wins when they are applied.
  REQUIRE(options.tunes.size() == 2);
  CHECK(options.tunes.at(0).value == 4096);
  CHECK(options.tunes.at(1).value == 8192);
}

TEST_CASE("A failed option leaves the options as they were",
          "[device-access][bench-options]") {
  BenchOptions options = ParseAll({L"--tune=a=1"});

  CHECK(Parse(L"--tune=b=oops", options).result == OptionResult::Malformed);
  CHECK(Parse(L"--stop-after=nope", options).result == OptionResult::Malformed);
  CHECK(Parse(L"--capture=", options).result == OptionResult::Malformed);

  CHECK(options.tunes.size() == 1);
  CHECK_FALSE(options.stop_after.has_value());
  CHECK(options.capture_path.empty());
}

TEST_CASE("BenchUsage lists every option", "[device-access][bench-options]") {
  const std::string_view usage = ToolDeviceAccess::BenchUsage();

  for (const std::string_view option :
       {"--benchmark", "--capture", "--stop-after", "--profile", "--disk",
        "--tune", "--media"}) {
    CHECK_THAT(std::string(usage), ContainsSubstring(std::string(option)));
  }
  CHECK(usage.ends_with('\n'));
}
