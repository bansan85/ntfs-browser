#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/device-access/temp-dir.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/test/mft/ntfsmftlist-sample-support.h>
#include <ntfs-browser/test/support/sample-run.h>
#include <ntfs-browser/tool/device-access/json.h>
#include <ntfs-browser/tuning/knobs.h>

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
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

// Slots of the image the benchmark runs list: files with a list of their own,
// so that both passes of the scan run.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

std::vector<BYTE> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

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

// The phase of the summary called name, or null.
const JsonValue* PhaseNamed(const JsonValue& run, std::string_view name) {
  for (const JsonValue& phase : Member(run, "phases").items) {
    if (Member(phase, "name").text == name) {
      return &phase;
    }
  }
  return nullptr;
}

std::wstring Option(std::wstring_view name, const std::filesystem::path& path) {
  return std::wstring(name) + L"=" + path.wstring();
}

std::vector<BYTE> ListImage() {
  return TestFakeImage::BuildFakeNtfsImageWithListMftScan(
      list_record_count, list_fragment_clusters);
}

// text with every spelling of path replaced by a placeholder: NtfsDir prints
// the path it was given, and two images are not at one path.
std::string WithoutPath(std::string text, const std::filesystem::path& path) {
  const std::string spelling = path.string();
  for (size_t at = text.find(spelling); at != std::string::npos;
       at = text.find(spelling, at)) {
    text.replace(at, spelling.size(), "<image>");
  }
  return text;
}

// Whether every byte the capture holds is the byte of the original at the same
// place: a capture holds the ranges the run read, and nothing else.
bool CaptureIsASubsetOf(const std::vector<BYTE>& capture,
                        const std::vector<BYTE>& original) {
  for (size_t index = 0; index < capture.size(); index++) {
    if (capture.at(index) != 0 && capture.at(index) != original.at(index)) {
      return false;
    }
  }
  return true;
}

// Checks the phases of a listing: each ran, in the order of the scan.
void CheckListingPhases(const JsonValue& run_json) {
  for (const std::string_view name :
       {"open", "pass1", "pass2", "link", "print"}) {
    INFO("phase " << name);
    const JsonValue* const phase = PhaseNamed(run_json, name);
    REQUIRE(phase != nullptr);
    CHECK(Count(Member(*phase, "count")) >= 1);
    CHECK(Count(Member(*phase, "wall_ns")) > 0);
  }
  CHECK(PhaseNamed(run_json, "walk") == nullptr);
  const std::uint64_t pass1_end =
      Count(Member(*PhaseNamed(run_json, "pass1"), "last_end_ns"));
  const std::uint64_t pass2_begin =
      Count(Member(*PhaseNamed(run_json, "pass2"), "first_begin_ns"));
  const std::uint64_t print_begin =
      Count(Member(*PhaseNamed(run_json, "print"), "first_begin_ns"));
  CHECK(pass1_end <= pass2_begin);
  CHECK(pass2_begin <= print_begin);
  CHECK(Count(Member(*PhaseNamed(run_json, "pass1"), "reads")) > 0);
}

// Checks that the reads of the kinds add up to the reads requested, and that
// the sizes and classes of each kind add up to its reads.
void CheckKindsAddUpTo(const JsonValue& kinds, std::uint64_t requested) {
  std::uint64_t kind_reads = 0;
  for (const auto& [name, kind] : kinds.members) {
    INFO("kind " << name);
    kind_reads += Count(Member(kind, "reads"));
    const JsonValue& sizes = Member(kind, "sizes");
    CHECK(Count(Member(sizes, "count")) == Count(Member(kind, "reads")));
    CHECK(Member(sizes, "buckets").items.size() == 40);
    std::uint64_t class_total = 0;
    for (const auto& [class_name, count] : Member(kind, "classes").members) {
      class_total += Count(count);
    }
    CHECK(class_total == Count(Member(kind, "reads")));
  }
  CHECK(kind_reads == requested);
  CHECK(Count(Member(Member(kinds, "mft_window"), "reads")) > 0);
  CHECK(Count(Member(Member(kinds, "boot"), "reads")) > 0);
  CHECK(Count(Member(Member(kinds, "attribute_list"), "reads")) > 0);
}

// Checks that the counters of the standing lane are there, and that nothing
// used the lane.
void CheckStandingLaneIsIdle(const JsonValue& run_json,
                             const JsonValue& reads) {
  for (const std::string_view name :
       {"standing_prefetch_calls", "standing_prefetch_bytes",
        "standing_cancel_calls"}) {
    INFO("reads." << name);
    CHECK(Count(Member(reads, name)) == 0);
  }
  for (const std::string_view name :
       {"standing_reads", "standing_bytes", "standing_wasted_bytes"}) {
    INFO("disk." << name);
    CHECK(Count(Member(Member(run_json, "disk"), name)) == 0);
  }
}

}  // namespace

TEST_CASE("NtfsMftList on its capture image lists what it listed on the image",
          "[device-access][capture][sample]") {
  const bool deleted = GENERATE(false, true);
  const bool big = GENERATE(false, true);
  INFO("deleted " << deleted << ", list image " << big);
  const std::vector<BYTE> original =
      big ? ListImage() : TestFakeImage::BuildFakeNtfsImageWithMftTree();
  const TestMft::TempTreeImage image(original);
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path capture = dir.Path() / "capture.img";
  std::vector<std::wstring> extra;
  if (deleted) {
    extra.emplace_back(L"--deleted");
  }
  const auto args = [&](const std::filesystem::path& target,
                        std::vector<std::wstring> more = {}) {
    std::vector<std::wstring> all = extra;
    all.insert(all.end(), more.begin(), more.end());
    all.push_back(target.wstring());
    return all;
  };

  const SampleRun plain = TestMft::RunMftList(args(image.path));
  REQUIRE(plain.exit_code == 0);
  REQUIRE_FALSE(plain.out.empty());
  const SampleRun captured =
      TestMft::RunMftList(args(image.path, {Option(L"--capture", capture)}));
  CHECK(captured.exit_code == 0);
  CHECK(captured.out == plain.out);
  CHECK(captured.err == plain.err);

  REQUIRE(std::filesystem::exists(capture));
  CHECK(std::filesystem::file_size(capture) == original.size());
  CHECK(CaptureIsASubsetOf(ReadFile(capture), original));

  // The capture is an image of the ranges the run used: listing it gives the
  // same listing.
  const SampleRun again = TestMft::RunMftList(args(capture));
  CHECK(again.exit_code == 0);
  CHECK(again.out == plain.out);
  CHECK(again.err == plain.err);
}

TEST_CASE("NtfsDir on its capture image lists what it listed on the image",
          "[device-access][capture][sample]") {
  const bool deleted = GENERATE(false, true);
  INFO("deleted " << deleted);
  const std::vector<BYTE> original =
      TestFakeImage::BuildFakeNtfsImageWithReportName(
          L"report.txt", TestFakeImage::dump_broken_alter_time);
  const TestIo::TempImage image(original);
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path capture = dir.Path() / "capture.img";
  std::vector<std::wstring> extra{L"--recursive"};
  if (deleted) {
    extra.emplace_back(L"--deleted");
  }

  for (const std::filesystem::path& exe : TestIo::SampleExes()) {
    INFO("sample " << exe.filename().string());
    const auto run = [&](const std::filesystem::path& target,
                         std::vector<std::wstring> more = {}) {
      std::vector<std::wstring> args = extra;
      args.insert(args.end(), more.begin(), more.end());
      args.push_back(target.wstring());
      return TestIo::RunDir(exe, args);
    };

    const SampleRun plain = run(image.path);
    REQUIRE(plain.exit_code == 0);
    REQUIRE_THAT(plain.out, ContainsSubstring("report.txt"));
    std::filesystem::remove(capture);
    const SampleRun captured = run(image.path, {Option(L"--capture", capture)});
    CHECK(captured.exit_code == 0);
    CHECK(captured.out == plain.out);
    CHECK(captured.err == plain.err);

    REQUIRE(std::filesystem::exists(capture));
    CHECK(std::filesystem::file_size(capture) == original.size());
    CHECK(CaptureIsASubsetOf(ReadFile(capture), original));

    const SampleRun again = run(capture);
    CHECK(again.exit_code == 0);
    CHECK(WithoutPath(again.out, capture) ==
          WithoutPath(plain.out, image.path));
    CHECK(again.err == plain.err);
  }
}

TEST_CASE("NtfsMftList --benchmark writes a summary that parses",
          "[device-access][benchmark][sample]") {
  const TestMft::TempTreeImage image(ListImage());
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path summary = dir.Path() / "summary.json";

  const SampleRun plain = TestMft::RunMftList({image.path.wstring()});
  const SampleRun run = TestMft::RunMftList(
      {Option(L"--benchmark", summary), image.path.wstring()});

  // The benchmark leaves the listing alone, and says where its summary is.
  REQUIRE(run.exit_code == 0);
  CHECK(run.out == plain.out);
  CHECK_THAT(run.err, ContainsSubstring("Benchmark summary: "));
  CHECK_THAT(run.err, ContainsSubstring("summary.json"));

  const JsonValue root = LoadSummary(summary);
  CHECK(Count(Member(root, "format")) == 1);
  CHECK(Member(root, "tool").text == "NtfsMftList");
  CHECK(Member(root, "target").text == image.path.string());
  CHECK(Member(root, "stop_after").text.empty());
  CHECK(Member(root, "arguments").kind == JsonValue::Kind::Array);
  CHECK_FALSE(Member(root, "arguments").items.empty());

  // The tuning in force: every knob, once.
  const JsonValue& tuning = Member(root, "tuning");
  CHECK(tuning.members.size() == Tuning::Knobs().size());
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    CHECK(tuning.Find(knob.name) != nullptr);
  }

  const JsonValue& run_json = Member(root, "run");
  CHECK(Count(Member(run_json, "wall_ns")) > 0);
  CHECK(Count(Member(run_json, "volume_bytes")) == ListImage().size());

  CheckListingPhases(run_json);

  // The threads: the main one, first.
  const JsonValue& threads = Member(run_json, "threads");
  REQUIRE_FALSE(threads.items.empty());
  CHECK(Member(threads.items.front(), "role").text == "main");

  // The reads, and the kinds and classes they add up to.
  const JsonValue& reads = Member(run_json, "reads");
  const std::uint64_t requested = Count(Member(reads, "requested"));
  CHECK(requested > 0);
  CHECK(Count(Member(reads, "requested_bytes")) > 0);
  CHECK(Count(Member(reads, "events_logged")) >= requested);
  CHECK(Count(Member(reads, "events_dropped")) == 0);

  CheckKindsAddUpTo(Member(run_json, "kinds"), requested);

  const JsonValue& classes = Member(run_json, "classes");
  for (const std::string_view name :
       {"unclassified", "io", "window", "ready", "plan"}) {
    INFO("class " << name);
    CHECK(Member(Member(classes, "reads"), name).kind ==
          JsonValue::Kind::Number);
    CHECK(Member(Member(classes, "wait_ns"), name).kind ==
          JsonValue::Kind::Number);
  }

  // A file does not tell its classes: the disk side of the report is there,
  // and empty for the reader of an image on this platform.
  CHECK(Member(Member(run_json, "disk"), "queue_depth").items.size() == 65);
  CheckStandingLaneIsIdle(run_json, reads);
  CHECK(Member(run_json, "main_thread_ns").kind == JsonValue::Kind::Object);
}

TEST_CASE("NtfsMftList --media sets the media kind its summary reports",
          "[device-access][benchmark][sample]") {
  const TestMft::TempTreeImage image(ListImage());
  const TestDeviceAccess::TempDir dir;

  const auto kind_of = [&](const std::wstring& media) {
    const std::filesystem::path summary = dir.Path() / "summary.json";
    std::filesystem::remove(summary);
    const SampleRun run = TestMft::RunMftList(
        {Option(L"--benchmark", summary), media, image.path.wstring()});
    REQUIRE(run.exit_code == 0);
    return Member(Member(LoadSummary(summary), "run"), "media_kind").text;
  };

  CHECK(kind_of(L"--media=hdd") == "rotational");
  CHECK(kind_of(L"--media=ssd") == "solid_state");
  CHECK(kind_of(L"--media=unknown") == "unknown");
}

TEST_CASE("NtfsMftList refuses a --media it does not know",
          "[device-access][benchmark][sample]") {
  const TestMft::TempTreeImage image(ListImage());

  const SampleRun run =
      TestMft::RunMftList({L"--media=floppy", image.path.wstring()});

  CHECK(run.exit_code != 0);
  CHECK_THAT(run.err, ContainsSubstring("--media"));
}

TEST_CASE("The read log next to a summary is the log of its run",
          "[device-access][benchmark][sample]") {
  const TestMft::TempTreeImage image(ListImage());
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path summary = dir.Path() / "summary.json";

  const SampleRun run = TestMft::RunMftList(
      {Option(L"--benchmark", summary), image.path.wstring()});
  REQUIRE(run.exit_code == 0);

  const JsonValue root = LoadSummary(summary);
  const std::filesystem::path log_path = Member(root, "read_log").text;
  CHECK(log_path.filename() == "summary.json.readlog");
  std::string error;
  const std::optional<Bench::ReadLog> log =
      Bench::LoadReadLog(log_path, &error);
  INFO(error);
  REQUIRE(log.has_value());

  const JsonValue& reads = Member(Member(root, "run"), "reads");
  CHECK(log->events.size() == Count(Member(reads, "events_logged")));
  CHECK(log->volume_bytes == ListImage().size());
  const auto logged_reads = static_cast<std::uint64_t>(
      std::ranges::count_if(log->events, [](const Bench::LogEvent& event) {
        return event.type == Bench::EventType::Read;
      }));
  CHECK(logged_reads == Count(Member(reads, "requested")));
}

TEST_CASE("NtfsMftList --stop-after ends the run after the phase",
          "[device-access][benchmark][sample]") {
  const TestMft::TempTreeImage image(ListImage());
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path summary = dir.Path() / "summary.json";

  const SampleRun run =
      TestMft::RunMftList({Option(L"--benchmark", summary),
                           L"--stop-after=pass1", image.path.wstring()});

  // No tree and no listing, and the summary says where the run stopped.
  CHECK(run.exit_code == 0);
  CHECK(run.out.empty());
  const JsonValue root = LoadSummary(summary);
  CHECK(Member(root, "stop_after").text == "pass1");
  const JsonValue& run_json = Member(root, "run");
  CHECK(PhaseNamed(run_json, "open") != nullptr);
  CHECK(PhaseNamed(run_json, "pass1") != nullptr);
  CHECK(PhaseNamed(run_json, "pass2") == nullptr);
  CHECK(PhaseNamed(run_json, "link") == nullptr);
  CHECK(PhaseNamed(run_json, "print") == nullptr);
}

TEST_CASE("NtfsDir --benchmark walks its phases and keeps its listing",
          "[device-access][benchmark][sample]") {
  const TestIo::TempImage image;
  const TestDeviceAccess::TempDir dir;

  for (const std::filesystem::path& exe : TestIo::SampleExes()) {
    INFO("sample " << exe.filename().string());
    const std::filesystem::path summary = dir.Path() / "summary-dir.json";
    const std::wstring target = TestIo::InImage(image.path, L"Docs");
    const SampleRun plain = TestIo::RunDir(exe, {target});
    const SampleRun run =
        TestIo::RunDir(exe, {Option(L"--benchmark", summary), target});

    REQUIRE(run.exit_code == 0);
    CHECK(run.out == plain.out);
    const JsonValue root = LoadSummary(summary);
    const JsonValue& run_json = Member(root, "run");
    CHECK(PhaseNamed(run_json, "open") != nullptr);
    CHECK(PhaseNamed(run_json, "walk") != nullptr);
    CHECK(Count(Member(Member(run_json, "reads"), "requested")) > 0);
    CHECK(Count(Member(Member(Member(run_json, "kinds"), "boot"), "reads")) >
          0);
  }
}
