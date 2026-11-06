#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/knobs.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "bench-options.h"
#include "json.h"
#include "temp-dir.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NtfsDevice::AppliedTuning;
using NtfsDevice::ApplyTuning;
using NtfsDevice::BenchOptions;

namespace {

// A target that is neither a drive letter nor a device: an image file, whose
// disk the system does not identify. A test that names a disk gives --disk.
constexpr std::wstring_view image_target = L"disk-image.img";

constexpr std::string_view exact_key = "ACME Fast|SN1";

// A profile with a section for one disk and one for any other.
constexpr std::string_view full_profile = R"({
  "format": 1,
  "generator": "tests",
  "disks": {
    "ACME Fast|SN1": {
      "label": "fast", "notes": "a note", "solid_state": true,
      "common": {"mft.window_bytes": 8192, "reader.max_reads": 5},
      "M1": {"reader.max_reads": 4},
      "M2": {"reader.max_reads": 8, "mft.max_run_slots": 16}
    },
    "*": {"common": {"mft.window_bytes": 16384}}
  }
})";

// Everything one ApplyTuning() call gave back.
struct Applied {
  std::string failure;
  AppliedTuning applied;
  VolumeOptions options;
};

Applied Apply(const BenchOptions& bench,
              std::wstring_view target = image_target) {
  Applied result;
  result.failure = ApplyTuning(bench, target, result.options, result.applied);
  return result;
}

// The options of a run that names a profile text and a disk.
struct Profile {
  Tests::TempDir dir;
  BenchOptions bench;

  explicit Profile(std::string_view text, std::string_view disk = exact_key) {
    bench.profile_path = dir.Write("profile.json", text);
    bench.disk = std::string(disk);
  }

  [[nodiscard]] std::string File() const { return bench.profile_path.string(); }

  [[nodiscard]] Applied Run() const { return Apply(bench); }

  // The failure of the profile, which the test cannot go on without.
  [[nodiscard]] std::string Failure() const {
    const Applied result = Run();
    REQUIRE_FALSE(result.failure.empty());
    return result.failure;
  }
};

std::uint64_t
    ValueOf(const VolumeOptions& options, std::string_view name,
            Tuning::Pattern pattern = Tuning::Pattern::LargeSequential) {
  const Tuning::Knob* const knob = Tuning::FindKnob(name);
  REQUIRE(knob != nullptr);
  return knob->get(options, pattern);
}

}  // namespace

TEST_CASE("Without a profile or a --tune, ApplyTuning changes nothing",
          "[device-access][apply-tuning]") {
  const Applied result = Apply(BenchOptions{});

  CHECK(result.failure.empty());
  CHECK_FALSE(result.applied.profile_loaded);
  CHECK_FALSE(result.applied.section_found);
  CHECK(result.applied.disk_key.empty());
  CHECK(result.applied.applied.empty());
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    CHECK(knob.get(result.options, Tuning::Pattern::LargeSequential) ==
          knob.default_value);
  }
}

TEST_CASE("A profile section for the disk applies, common then each pattern",
          "[device-access][apply-tuning]") {
  const Profile profile(full_profile);

  const Applied result = profile.Run();

  REQUIRE(result.failure.empty());
  CHECK(result.applied.profile_loaded);
  CHECK(result.applied.section_found);
  CHECK(result.applied.disk_key == exact_key);
  // Common first, then M1 to M4: a pattern section overrides it.
  CHECK(result.applied.applied ==
        std::vector<std::string>{"mft.window_bytes=8192", "reader.max_reads=5",
                                 "reader.max_reads@M1=4",
                                 "reader.max_reads@M2=8",
                                 "mft.max_run_slots@M2=16"});
  CHECK(ValueOf(result.options, "mft.window_bytes") == 8192);
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::LargeSequential) == 4);
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::ScatteredGrouped) == 8);
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::DependentSmall) == 5);
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::LargeData) == 5);
  CHECK(ValueOf(result.options, "mft.max_run_slots") == 16);
  // What the profile leaves out stays as it was.
  CHECK(ValueOf(result.options, "probe.max_gap_bytes") ==
        Tuning::FindKnob("probe.max_gap_bytes")->default_value);
}

TEST_CASE("The section of the exact key wins over the star",
          "[device-access][apply-tuning]") {
  Profile profile(full_profile);

  const Applied exact = profile.Run();
  profile.bench.disk = "Other Disk|9";
  const Applied other = profile.Run();

  CHECK(exact.applied.disk_key == exact_key);
  CHECK(ValueOf(exact.options, "mft.window_bytes") == 8192);
  // A disk with no section of its own takes the star.
  REQUIRE(other.failure.empty());
  CHECK(other.applied.section_found);
  CHECK(other.applied.disk_key == "*");
  CHECK(ValueOf(other.options, "mft.window_bytes") == 16384);
  CHECK(other.applied.applied ==
        std::vector<std::string>{"mft.window_bytes=16384"});
}

TEST_CASE("The star serves a target whose disk the system does not name",
          "[device-access][apply-tuning]") {
  Profile profile(full_profile, "");

  // An image file has no firmware identity: the key is empty, and only the
  // star can match it.
  const Applied result = profile.Run();

  REQUIRE(result.failure.empty());
  CHECK(result.applied.disk_key == "*");
  CHECK(ValueOf(result.options, "mft.window_bytes") == 16384);
}

TEST_CASE("The option --disk names a section whatever the target is",
          "[device-access][apply-tuning]") {
  Profile profile(full_profile, exact_key);

  for (const std::wstring_view target :
       {std::wstring_view(L"disk-image.img"), std::wstring_view(L"other.img"),
        std::wstring_view(L"images/deep/disk.img")}) {
    const Applied result = Apply(profile.bench, target);
    REQUIRE(result.failure.empty());
    CHECK(result.applied.disk_key == exact_key);
  }
}

TEST_CASE("A profile with no section for the disk and no star applies nothing",
          "[device-access][apply-tuning]") {
  const Profile profile(R"({"format": 1, "disks": {"Only|One": {
      "common": {"mft.window_bytes": 8192}}}})",
                        "Not|There");

  const Applied result = profile.Run();

  CHECK(result.failure.empty());
  CHECK(result.applied.profile_loaded);
  CHECK_FALSE(result.applied.section_found);
  CHECK(result.applied.disk_key.empty());
  CHECK(result.applied.applied.empty());
  CHECK(ValueOf(result.options, "mft.window_bytes") ==
        Tuning::FindKnob("mft.window_bytes")->default_value);
}

TEST_CASE("A profile of no disks is a profile with no section",
          "[device-access][apply-tuning]") {
  const Profile profile(R"({"format": 1, "disks": {}})");

  const Applied result = profile.Run();

  CHECK(result.failure.empty());
  CHECK(result.applied.profile_loaded);
  CHECK_FALSE(result.applied.section_found);
}

TEST_CASE("A --tune comes after the profile and overrides it",
          "[device-access][apply-tuning]") {
  Profile profile(full_profile);
  profile.bench.tunes.push_back({.name = "mft.window_bytes", .value = 4096});
  profile.bench.tunes.push_back({.name = "reader.max_reads",
                                 .pattern = Tuning::Pattern::LargeData,
                                 .value = 2});

  const Applied result = profile.Run();

  REQUIRE(result.failure.empty());
  CHECK(ValueOf(result.options, "mft.window_bytes") == 4096);
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::LargeData) == 2);
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::DependentSmall) == 5);
  REQUIRE(result.applied.applied.size() == 7);
  CHECK(result.applied.applied.at(5) == "mft.window_bytes=4096");
  CHECK(result.applied.applied.at(6) == "reader.max_reads@M4=2");
}

TEST_CASE("A --tune alone applies without a profile",
          "[device-access][apply-tuning]") {
  BenchOptions bench;
  bench.tunes.push_back({.name = "probe.max_run_bytes", .value = 8192});
  bench.tunes.push_back({.name = "probe.max_run_bytes", .value = 16384});

  const Applied result = Apply(bench);

  REQUIRE(result.failure.empty());
  CHECK_FALSE(result.applied.profile_loaded);
  CHECK(result.applied.disk_key.empty());
  CHECK(ValueOf(result.options, "probe.max_run_bytes") == 16384);
  CHECK(result.applied.applied ==
        std::vector<std::string>{"probe.max_run_bytes=8192",
                                 "probe.max_run_bytes=16384"});
}

TEST_CASE("A --tune the table refuses names its reason",
          "[device-access][apply-tuning]") {
  const auto failure_of = [](NtfsDevice::TuneSetting tune) {
    BenchOptions bench;
    bench.tunes.push_back(std::move(tune));
    const Applied result = Apply(bench);
    REQUIRE_FALSE(result.failure.empty());
    return result.failure;
  };

  CHECK(failure_of({.name = "mft.nothing", .value = 1}) ==
        "--tune: unknown key \"mft.nothing\"");
  CHECK_THAT(
      failure_of({.name = "mft.window_bytes", .value = 1}),
      ContainsSubstring("--tune: \"mft.window_bytes\" is 1, outside 4096"));
  CHECK(failure_of({.name = "mft.window_bytes",
                    .pattern = Tuning::Pattern::ScatteredGrouped,
                    .value = 8192}) ==
        "--tune: \"mft.window_bytes\" does not serve pattern M2");
}

TEST_CASE("Every knob of the table can be set from a profile",
          "[device-access][apply-tuning]") {
  NtfsDevice::JsonWriter json;
  json.BeginObject();
  json.Field("format", std::uint64_t{1});
  json.Key("disks");
  json.BeginObject();
  json.Key("*");
  json.BeginObject();
  json.Key("common");
  json.BeginObject();
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    json.Field(knob.name, knob.min_value);
  }
  json.EndObject();
  json.EndObject();
  json.EndObject();
  json.EndObject();
  const Profile profile(json.Take(), "any");

  const Applied result = profile.Run();

  REQUIRE(result.failure.empty());
  CHECK(result.applied.applied.size() == Tuning::Knobs().size());
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    INFO("knob " << knob.name);
    for (std::size_t index = 0; index < Tuning::pattern_count; index++) {
      if ((knob.patterns & (1U << index)) != 0) {
        CHECK(knob.get(result.options, static_cast<Tuning::Pattern>(index)) ==
              knob.min_value);
      }
    }
  }
}

TEST_CASE("A knob set in a pattern section reaches that pattern only",
          "[device-access][apply-tuning]") {
  const Profile profile(R"({"format": 1, "disks": {"*": {
      "M3": {"reader.max_reads": 3, "cache.block_bytes": 8192},
      "M4": {"check.max_read_bytes": 8192}}}})",
                        "any");

  const Applied result = profile.Run();

  REQUIRE(result.failure.empty());
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::DependentSmall) == 3);
  CHECK(ValueOf(result.options, "reader.max_reads",
                Tuning::Pattern::LargeSequential) == 32);
  CHECK(ValueOf(result.options, "cache.block_bytes") == 8192);
  CHECK(ValueOf(result.options, "check.max_read_bytes") == 8192);
}

TEST_CASE("The profile loader refuses a file it cannot use, naming it",
          "[device-access][apply-tuning]") {
  SECTION("a file that is not there") {
    BenchOptions bench;
    bench.profile_path = std::filesystem::path("no/such/profile.json");
    bench.disk = "any";
    CHECK(Apply(bench).failure ==
          "cannot read the profile " + bench.profile_path.string());
  }

  SECTION("a document that is not JSON") {
    const Profile profile("{\"format\": 1,");
    const std::string failure = profile.Failure();
    CHECK_THAT(failure, ContainsSubstring(profile.File() + ": "));
    CHECK_THAT(failure, ContainsSubstring("(at byte "));
  }

  SECTION("a key that appears twice") {
    const Profile profile(R"({"format": 1, "format": 1, "disks": {}})");
    CHECK_THAT(profile.Failure(),
               ContainsSubstring(profile.File() +
                                 ": the key \"format\" appears twice"));
  }

  SECTION("a document that is not an object") {
    const Profile profile("[1, 2]");
    CHECK(profile.Failure() ==
          profile.File() + ": the profile is not an object");
  }

  SECTION("a top-level key that is unknown") {
    const Profile profile(R"({"format": 1, "disks": {}, "disk": {}})");
    CHECK(profile.Failure() == profile.File() + ": unknown key \"disk\"");
  }

  SECTION("a format that is missing or not 1") {
    for (const std::string_view text :
         {R"({"disks": {}})", R"({"format": 2, "disks": {}})",
          R"({"format": "1", "disks": {}})",
          R"({"format": 1.5, "disks": {}})"}) {
      const Profile profile(text);
      CHECK(profile.Failure() == profile.File() + ": \"format\" MUST be 1");
    }
  }

  SECTION("disks that are missing or not an object") {
    for (const std::string_view text :
         {R"({"format": 1})", R"({"format": 1, "disks": []})",
          R"({"format": 1, "disks": "all"})"}) {
      const Profile profile(text);
      CHECK(profile.Failure() ==
            profile.File() + ": \"disks\" MUST be an object");
    }
  }
}

TEST_CASE("The profile loader refuses a disk section, naming its place",
          "[device-access][apply-tuning]") {
  SECTION("a section that is not an object") {
    const Profile profile(R"({"format": 1, "disks": {"Some|Disk": 5}})");
    CHECK(profile.Failure() ==
          profile.File() + ": disks[\"Some|Disk\"] is not an object");
  }

  SECTION("a key a section does not have") {
    const Profile profile(
        R"({"format": 1, "disks": {"ACME Fast|SN1": {"comon": {}}}})");
    CHECK(profile.Failure() ==
          profile.File() + ": disks[\"ACME Fast|SN1\"]: unknown key \"comon\"");
  }

  SECTION("a pattern the library does not have") {
    const Profile profile(
        R"({"format": 1, "disks": {"ACME Fast|SN1": {"M5": {}}}})");
    CHECK_THAT(profile.Failure(), ContainsSubstring("unknown key \"M5\""));
  }

  SECTION("a section of another disk is checked as well") {
    const Profile profile(R"({"format": 1, "disks": {
        "ACME Fast|SN1": {"common": {"mft.window_bytes": 8192}},
        "Unused|Disk": {"M9": {}}}})");
    CHECK(profile.Failure() ==
          profile.File() + ": disks[\"Unused|Disk\"]: unknown key \"M9\"");
  }

  SECTION("a common or pattern section that is not an object") {
    const Profile common(
        R"({"format": 1, "disks": {"ACME Fast|SN1": {"common": [1]}}})");
    CHECK(common.Failure() ==
          common.File() + ": disks[\"ACME Fast|SN1\"].common is not an object");
    const Profile pattern(
        R"({"format": 1, "disks": {"ACME Fast|SN1": {"M2": 3}}})");
    CHECK(pattern.Failure() ==
          pattern.File() + ": disks[\"ACME Fast|SN1\"].M2 is not an object");
  }
}

TEST_CASE("The profile loader refuses a value that is not a whole number",
          "[device-access][apply-tuning]") {
  for (const std::string_view value :
       {"1.5", "-1", "\"8192\"", "true", "null", "[8192]", "{}", "1e300",
        "-0.5", "4096.0000001"}) {
    INFO("value " << value);
    const Profile profile(
        R"({"format": 1, "disks": {"ACME Fast|SN1": {"common": {"mft.window_bytes": )" +
        std::string(value) + "}}}}");
    CHECK(profile.Failure() ==
          profile.File() +
              ": disks[\"ACME Fast|SN1\"].common.mft.window_bytes is not a "
              "whole number");
  }
  // 4096.0 and 8192e0 are whole numbers.
  for (const std::string_view value : {"4096.0", "8192e0", "0.4096e4"}) {
    INFO("value " << value);
    const Profile profile(
        R"({"format": 1, "disks": {"ACME Fast|SN1": {"M1": {"mft.window_bytes": )" +
        std::string(value) + "}}}}");
    CHECK(profile.Run().failure.empty());
  }
}

TEST_CASE("The profile loader refuses a value outside the bounds of its knob",
          "[device-access][apply-tuning]") {
  const Profile profile(
      R"({"format": 1, "disks": {"ACME Fast|SN1": {"M2": {"probe.max_chunk_records": 0}}}})");

  CHECK(profile.Failure() ==
        profile.File() +
            ": disks[\"ACME Fast|SN1\"].M2: "
            "\"probe.max_chunk_records\" is 0, outside 1 to 1048576");

  const Profile huge(
      R"({"format": 1, "disks": {"*": {"common": {"mft.window_bytes": 9007199254740992}}}})",
      "any");
  CHECK_THAT(huge.Failure(),
             ContainsSubstring("disks[\"*\"].common: \"mft.window_bytes\" is "
                               "9007199254740992, outside 4096 to"));
}

TEST_CASE("The profile loader refuses a pattern its knob does not serve",
          "[device-access][apply-tuning]") {
  const Profile profile(
      R"({"format": 1, "disks": {"ACME Fast|SN1": {"M3": {"mft.window_bytes": 8192}}}})");

  CHECK(profile.Failure() ==
        profile.File() +
            ": disks[\"ACME Fast|SN1\"].M3: "
            "\"mft.window_bytes\" does not serve pattern M3");
}

TEST_CASE("The profile loader refuses a name that is not a tunable constant",
          "[device-access][apply-tuning]") {
  // Family D, the guard rails that cap a forged input, has no name at all:
  // nothing in a profile reaches them.
  for (const std::string_view name :
       {"mft.nothing", "max_orphan_scan_blocks", "guard.max_depth",
        "mft.max_memoized_path_units", "alloc.max_bad_extents",
        "usn.max_records", "mft.window_bytes ", "MFT.WINDOW_BYTES", ""}) {
    INFO("name \"" << name << "\"");
    const Profile profile(
        R"({"format": 1, "disks": {"ACME Fast|SN1": {"common": {")" +
        std::string(name) + R"(": 4096}}}})");
    CHECK(profile.Failure() ==
          profile.File() + ": disks[\"ACME Fast|SN1\"].common: unknown key \"" +
              std::string(name) +
              "\" (only constants of families A, B and C can be set)");
  }
}

TEST_CASE("A refusal comes before the knobs after it are applied",
          "[device-access][apply-tuning]") {
  // The loader stops at the first bad entry: an entry before it was applied,
  // the ones after it were not.
  const Profile profile(
      R"({"format": 1, "disks": {"ACME Fast|SN1": {"common": {
      "mft.window_bytes": 8192, "nope": 1, "mft.max_run_slots": 9}}}})");

  const Applied result = profile.Run();

  CHECK_FALSE(result.failure.empty());
  CHECK(ValueOf(result.options, "mft.window_bytes") == 8192);
  CHECK(ValueOf(result.options, "mft.max_run_slots") ==
        Tuning::FindKnob("mft.max_run_slots")->default_value);
  CHECK(result.applied.profile_loaded);
}

TEST_CASE("The counts of runs of the standing lane load from a profile",
          "[device-access][apply-tuning]") {
  const Profile profile(R"({"format": 1, "disks": {"*": {
      "M3": {"walk.read_ahead_runs": 16,
             "walk.read_ahead_runs_rotational": 4}}}})",
                        "any");

  const Applied result = profile.Run();

  REQUIRE(result.failure.empty());
  CHECK(result.options.tuning.walk.read_ahead_runs == 16);
  CHECK(result.options.tuning.walk.read_ahead_runs_rotational == 4);
  // The other constants of the walk keep their default.
  CHECK(result.options.tuning.walk.read_ahead_dirs == 64);
}

TEST_CASE(
    "A profile refuses the counts of runs of the standing lane out of "
    "bounds or for another pattern",
    "[device-access][apply-tuning]") {
  const Profile too_many(
      R"({"format": 1, "disks": {"*": {"M3": {"walk.read_ahead_runs": 4097}}}})",
      "any");
  CHECK_THAT(too_many.Failure(), ContainsSubstring("walk.read_ahead_runs"));

  const Profile wrong_pattern(
      R"({"format": 1, "disks": {"*": {"M1": {"walk.read_ahead_runs_rotational": 4}}}})",
      "any");
  CHECK_THAT(wrong_pattern.Failure(),
             ContainsSubstring("does not serve pattern M1"));
}
