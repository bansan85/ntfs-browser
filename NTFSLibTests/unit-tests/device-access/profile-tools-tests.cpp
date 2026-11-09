#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include <ntfs-browser/test/device-access/temp-dir.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/test/mft/ntfsscanbench-sample-support.h>
#include <ntfs-browser/test/support/sample-run.h>
#include <ntfs-browser/tool/device-access/json.h>

#include "tuning/knobs.h"
#include "win-types.h"

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

namespace TestIo = NTFS_BROWSER_NAMESPACE::Test::Io;
namespace TestMft = NTFS_BROWSER_NAMESPACE::Test::Mft;
namespace TestDeviceAccess = NTFS_BROWSER_NAMESPACE::Test::DeviceAccess;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using Catch::Matchers::ContainsSubstring;
using TestSupport::SampleRun;
using ToolDeviceAccess::JsonValue;

namespace {

// Slots of the image NtfsScanBench lists: files with a list of their own, so
// that both passes of the scan run.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// The disk the profiles of these tests are for. Always given with --disk:
// the tools would otherwise ask the system who the disk behind the temp
// folder is.
constexpr std::wstring_view disk_option = L"--disk=Test Disk|1";
constexpr std::string_view disk_key = "Test Disk|1";

// A profile with a section for the disk, and the star for any other.
constexpr std::string_view tuned_profile = R"({
  "format": 1,
  "disks": {
    "Test Disk|1": {
      "common": {"output.flush_bytes": 4096, "probe.max_chunk_records": 2,
                 "mft.window_bytes": 8192},
      "M1": {"reader.max_reads": 3, "mft.max_gap_slots": 4},
      "M2": {"reader.max_reads": 8, "cache.block_bytes": 65536}
    },
    "*": {"common": {"mft.window_bytes": 65536}}
  }
})";

// The settings of tuned_profile, in the order a run applies them.
const std::vector<std::string> tuned_applied{
    "output.flush_bytes=4096",   "probe.max_chunk_records=2",
    "mft.window_bytes=8192",     "reader.max_reads@M1=3",
    "mft.max_gap_slots@M1=4",    "reader.max_reads@M2=8",
    "cache.block_bytes@M2=65536"};

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Parses the summary at path, which the test cannot go on without.
JsonValue LoadSummary(const std::filesystem::path& path) {
  REQUIRE(std::filesystem::exists(path));
  std::string error;
  std::optional<JsonValue> parsed =
      ToolDeviceAccess::ParseJson(ReadText(path), error);
  INFO("parse error: " << error);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

// The member of an object, which the test cannot go on without.
const JsonValue& Member(const JsonValue& object, std::string_view key) {
  REQUIRE(object.kind == JsonValue::Kind::Object);
  const JsonValue* const found = object.Find(key);
  INFO("member " << key);
  REQUIRE(found != nullptr);
  return *found;
}

std::uint64_t Count(const JsonValue& value) {
  REQUIRE(value.kind == JsonValue::Kind::Number);
  return static_cast<std::uint64_t>(value.number);
}

// The reads the run of a summary asked of the volume.
std::uint64_t RequestedReads(const JsonValue& summary) {
  return Count(Member(Member(Member(summary, "run"), "reads"), "requested"));
}

std::vector<std::string> Applied(const JsonValue& summary) {
  std::vector<std::string> applied;
  for (const JsonValue& setting :
       Member(Member(summary, "profile"), "applied").items) {
    applied.push_back(setting.text);
  }
  return applied;
}

std::wstring Option(std::wstring_view name, const std::filesystem::path& path) {
  return std::wstring(name) + L"=" + path.wstring();
}

// One tool, as the tests run it: its listing arguments, and how to start it.
struct Tool {
  std::string name;
  // The engine option the tool takes, or empty for the default.
  std::wstring engine;
  // The image and the target the listing names.
  std::filesystem::path image;
  std::wstring target;

  [[nodiscard]] SampleRun Run(std::vector<std::wstring> options) const {
    if (!engine.empty()) {
      options.push_back(engine);
    }
    options.push_back(target);
    return TestMft::RunScanBench(options);
  }
};

// The image files the tools list, made once per test.
struct Images {
  TestMft::TempTreeImage mft_list{
      TestFakeImage::BuildFakeNtfsImageWithListMftScan(list_record_count,
                                                       list_fragment_clusters)};
  TestIo::TempImage dir;
};

// Every tool of the build, over the image of its listing.
std::vector<Tool> Tools(const Images& images) {
  std::vector<Tool> tools;
  tools.push_back({.name = "mftlist",
                   .engine = L"--engine=mftlist",
                   .image = images.mft_list.path,
                   .target = images.mft_list.path.wstring()});
  for (const std::string_view engine : {"nocache", "fullcache"}) {
    tools.push_back(
        {.name = std::string(engine),
         .engine = L"--engine=" + std::wstring(engine.begin(), engine.end()),
         .image = images.dir.path,
         .target = TestIo::InImage(images.dir.path, L"Docs")});
  }
  return tools;
}

}  // namespace

TEST_CASE("A tool run with a profile lists what it lists without",
          "[device-access][profile][sample]") {
  const Images images;
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path profile =
      dir.Write("profile.json", tuned_profile);

  for (const Tool& tool : Tools(images)) {
    INFO("tool " << tool.name);
    const SampleRun plain = tool.Run({});
    REQUIRE(plain.exit_code == 0);
    REQUIRE_FALSE(plain.out.empty());

    const SampleRun tuned =
        tool.Run({Option(L"--profile", profile), std::wstring(disk_option)});
    CHECK(tuned.exit_code == 0);
    CHECK(tuned.out == plain.out);
    CHECK(tuned.err == plain.err);
  }
}

TEST_CASE("A tool run with --tune lists what it lists without",
          "[device-access][profile][sample]") {
  const Images images;

  for (const Tool& tool : Tools(images)) {
    INFO("tool " << tool.name);
    const SampleRun plain = tool.Run({});
    REQUIRE(plain.exit_code == 0);

    const SampleRun tuned = tool.Run(
        {L"--tune=output.flush_bytes=4096", L"--tune=probe.max_run_bytes=4096",
         L"--tune=reader.max_reads@M2=1", L"--tune=cache.block_bytes=4096",
         L"--tune=mft.window_bytes=4096",
         L"--tune=mft.max_held_record_bytes=0"});
    CHECK(tuned.exit_code == 0);
    CHECK(tuned.out == plain.out);
    CHECK(tuned.err == plain.err);
  }
}

TEST_CASE(
    "The summary of a tuned run shows the values and the reads they changed",
    "[device-access][profile][sample]") {
  const Images images;
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path profile =
      dir.Write("profile.json", tuned_profile);

  for (const Tool& tool : Tools(images)) {
    INFO("tool " << tool.name);
    const std::filesystem::path plain_summary =
        dir.Path() / (tool.name + "-plain.json");
    const std::filesystem::path tuned_summary =
        dir.Path() / (tool.name + "-tuned.json");
    REQUIRE(tool.Run({Option(L"--benchmark", plain_summary)}).exit_code == 0);
    const SampleRun tuned =
        tool.Run({Option(L"--benchmark", tuned_summary),
                  Option(L"--profile", profile), std::wstring(disk_option)});
    REQUIRE(tuned.exit_code == 0);

    const JsonValue plain = LoadSummary(plain_summary);
    const JsonValue summary = LoadSummary(tuned_summary);

    // Where the settings came from.
    const JsonValue& from = Member(summary, "profile");
    CHECK(Member(from, "path").text == profile.string());
    CHECK(Member(from, "loaded").boolean);
    CHECK(Member(from, "section_found").boolean);
    CHECK(Member(from, "disk").text == disk_key);
    CHECK(Applied(summary) == tuned_applied);
    CHECK_FALSE(Member(Member(plain, "profile"), "loaded").boolean);
    CHECK(Applied(plain).empty());

    // The values in force.
    const JsonValue& tuning = Member(summary, "tuning");
    CHECK(Count(Member(tuning, "output.flush_bytes")) == 4096);
    CHECK(Count(Member(tuning, "probe.max_chunk_records")) == 2);
    CHECK(Count(Member(tuning, "mft.window_bytes")) == 8192);
    CHECK(Count(Member(tuning, "mft.max_gap_slots")) == 4);
    CHECK(Count(Member(tuning, "cache.block_bytes")) == 65536);
    const JsonValue& max_reads = Member(tuning, "reader.max_reads");
    CHECK(Count(Member(max_reads, "M1")) == 3);
    CHECK(Count(Member(max_reads, "M2")) == 8);
    CHECK(Count(Member(max_reads, "M3")) == 32);
    CHECK(Count(Member(max_reads, "M4")) == 32);
    CHECK(Count(Member(Member(plain, "tuning"), "mft.window_bytes")) ==
          Tuning::FindKnob("mft.window_bytes")->default_value);

    // The reads they changed. A smaller window reads $MFT in more pieces; a
    // larger block reads the cache in fewer.
    if (tool.name == "mftlist") {
      CHECK(RequestedReads(summary) > RequestedReads(plain));
    } else if (tool.name == "fullcache") {
      CHECK(RequestedReads(summary) != RequestedReads(plain));
    }
  }
}

TEST_CASE(
    "The option --tune comes after the profile in a tool and in its summary",
    "[device-access][profile][sample]") {
  const Images images;
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path profile =
      dir.Write("profile.json", tuned_profile);
  const Tool tool = Tools(images).front();
  const std::filesystem::path summary_path = dir.Path() / "summary.json";

  const SampleRun run = tool.Run(
      {Option(L"--benchmark", summary_path), L"--tune=mft.window_bytes=4096",
       Option(L"--profile", profile), std::wstring(disk_option)});

  REQUIRE(run.exit_code == 0);
  const JsonValue summary = LoadSummary(summary_path);
  CHECK(Count(Member(Member(summary, "tuning"), "mft.window_bytes")) == 4096);
  // The profile first, whatever the order of the command line.
  std::vector<std::string> expected = tuned_applied;
  expected.emplace_back("mft.window_bytes=4096");
  CHECK(Applied(summary) == expected);
}

TEST_CASE("A profile without a section for the disk is said and ignored",
          "[device-access][profile][sample]") {
  const Images images;
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path profile =
      dir.Write("profile.json", R"({"format": 1, "disks": {"Only|One": {
          "common": {"mft.window_bytes": 8192}}}})");

  for (const Tool& tool : Tools(images)) {
    INFO("tool " << tool.name);
    const std::filesystem::path summary_path =
        dir.Path() / (tool.name + "-summary.json");
    const SampleRun plain = tool.Run({});
    const SampleRun run =
        tool.Run({Option(L"--profile", profile), std::wstring(disk_option),
                  Option(L"--benchmark", summary_path)});

    CHECK(run.exit_code == 0);
    CHECK(run.out == plain.out);
    CHECK_THAT(run.err, ContainsSubstring("The profile has no section for "
                                          "this disk: defaults used"));
    const JsonValue summary = LoadSummary(summary_path);
    CHECK(Member(Member(summary, "profile"), "loaded").boolean);
    CHECK_FALSE(Member(Member(summary, "profile"), "section_found").boolean);
    CHECK(Applied(summary).empty());
  }
}

TEST_CASE("The star of a profile serves a disk the profile does not name",
          "[device-access][profile][sample]") {
  const Images images;
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path profile =
      dir.Write("profile.json", tuned_profile);
  const Tool tool = Tools(images).front();
  const std::filesystem::path summary_path = dir.Path() / "summary.json";

  const SampleRun run =
      tool.Run({Option(L"--profile", profile), L"--disk=Another Disk|2",
                Option(L"--benchmark", summary_path)});

  REQUIRE(run.exit_code == 0);
  const JsonValue summary = LoadSummary(summary_path);
  CHECK(Member(Member(summary, "profile"), "disk").text == "*");
  CHECK(Applied(summary) == std::vector<std::string>{"mft.window_bytes=65536"});
  CHECK(Count(Member(Member(summary, "tuning"), "mft.window_bytes")) == 65536);
}

TEST_CASE("A profile a tool cannot use ends the run with its reason",
          "[device-access][profile][sample]") {
  struct Case {
    std::string name;
    std::string text;
    std::string message;
  };

  const std::vector<Case> cases{
      {"unknown knob",
       R"({"format": 1, "disks": {"Test Disk|1": {"common": {"mft.nothing": 1}}}})",
       "disks[\"Test Disk|1\"].common: unknown key \"mft.nothing\""},
      {"guard rail",
       R"({"format": 1, "disks": {"Test Disk|1": {"common": {"max_orphan_scan_blocks": 1}}}})",
       "only constants of families A, B and C can be set"},
      {"out of range",
       R"({"format": 1, "disks": {"Test Disk|1": {"M1": {"mft.window_bytes": 1}}}})",
       "disks[\"Test Disk|1\"].M1: \"mft.window_bytes\" is 1, outside 4096 to"},
      {"wrong pattern",
       R"({"format": 1, "disks": {"Test Disk|1": {"M2": {"mft.window_bytes": 8192}}}})",
       "\"mft.window_bytes\" does not serve pattern M2"},
      {"not a whole number",
       R"({"format": 1, "disks": {"Test Disk|1": {"common": {"mft.window_bytes": 1.5}}}})",
       "common.mft.window_bytes is not a whole number"},
      {"not JSON", "{\"format\": 1,", "(at byte "},
      {"wrong format", R"({"format": 3, "disks": {}})",
       "\"format\" MUST be 1"}};

  const Images images;
  const TestDeviceAccess::TempDir dir;
  for (const Case& bad : cases) {
    const std::filesystem::path profile = dir.Write("bad.json", bad.text);
    for (const Tool& tool : Tools(images)) {
      INFO("profile " << bad.name << ", tool " << tool.name);
      const SampleRun run =
          tool.Run({Option(L"--profile", profile), std::wstring(disk_option)});

      CHECK(run.exit_code == TestSupport::sample_failure_exit_code);
      // No listing: the run ended before it opened the volume.
      CHECK(run.out.empty());
      CHECK_THAT(run.err, ContainsSubstring(profile.string()));
      CHECK_THAT(run.err, ContainsSubstring(bad.message));
    }
  }
}

TEST_CASE("A profile that is not there ends the run",
          "[device-access][profile][sample]") {
  const Images images;
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path missing = dir.Path() / "missing.json";

  for (const Tool& tool : Tools(images)) {
    INFO("tool " << tool.name);
    const SampleRun run =
        tool.Run({Option(L"--profile", missing), std::wstring(disk_option)});

    CHECK(run.exit_code == TestSupport::sample_failure_exit_code);
    CHECK(run.out.empty());
    CHECK_THAT(run.err, ContainsSubstring("cannot read the profile " +
                                          missing.string()));
  }
}

TEST_CASE("A --tune a tool cannot use ends the run with its reason",
          "[device-access][profile][sample]") {
  const Images images;

  struct Case {
    std::wstring option;
    std::string message;
  };

  const std::vector<Case> cases{
      {L"--tune=mft.nothing=1", "--tune: unknown key \"mft.nothing\""},
      {L"--tune=mft.window_bytes=1",
       "--tune: \"mft.window_bytes\" is 1, outside 4096 to"},
      {L"--tune=mft.window_bytes@M2=8192",
       "--tune: \"mft.window_bytes\" does not serve pattern M2"},
      {L"--tune=mft.window_bytes@M9=8192", "it is M1, M2, M3 or M4"},
      {L"--tune=mft.window_bytes", "--tune expects name=value"},
      {L"--tune=mft.window_bytes=abc", "a whole number after the equals sign"}};

  for (const Case& bad : cases) {
    for (const Tool& tool : Tools(images)) {
      INFO("option " << bad.message << ", tool " << tool.name);
      const SampleRun run = tool.Run({bad.option});

      CHECK(run.exit_code == TestSupport::sample_failure_exit_code);
      CHECK_THAT(run.err, ContainsSubstring(bad.message));
    }
  }
}

TEST_CASE("A malformed option of the benchmark ends the run with its reason",
          "[device-access][profile][sample]") {
  const Images images;

  struct Case {
    std::wstring option;
    std::string message;
  };

  const std::vector<Case> cases{
      {L"--benchmark=", "--benchmark= needs a path"},
      {L"--capture=", "--capture needs a path"},
      {L"--profile=", "--profile needs a path"},
      {L"--stop-after=nowhere", "--stop-after names the phase \"nowhere\""}};

  for (const Case& bad : cases) {
    for (const Tool& tool : Tools(images)) {
      INFO("option " << bad.message << ", tool " << tool.name);
      const SampleRun run = tool.Run({bad.option});

      CHECK(run.exit_code == TestSupport::sample_failure_exit_code);
      CHECK_THAT(run.err, ContainsSubstring(bad.message));
    }
  }
}
