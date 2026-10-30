#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/knobs.h>

#include "fake-ntfs-image.h"
#include "json.h"
#include "ntfsmftlist-sample-support.h"
#include "sample-run.h"
#include "temp-dir.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace MftListSample = NTFS_BROWSER_NAMESPACE::Tests::MftListSample;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using Catch::Matchers::ContainsSubstring;
using NtfsDevice::JsonValue;
using Tests::SampleRun;

namespace {

// Slots of the image whose run the replay tests record: files with a list of
// their own, so that both passes of the scan run.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// Knobs the library lists: the benchmark script reads the table.
constexpr std::size_t knob_count = 44;

// Bytes of the plain file the diskbench tests measure: the least a zone may
// hold is twice a 4 MiB sequential read.
constexpr std::size_t measured_file_bytes = std::size_t{16} << 20U;

// Results a disk sheet holds: three patterns at six queue depths.
constexpr std::size_t sheet_results = 18;

// Not a drive and not a device, and no such file: no tool takes it for a disk.
constexpr std::wstring_view absent_target = L"ntfs-bench-test-no-such-target";

SampleRun RunBench(const std::vector<std::wstring>& args) {
  const std::filesystem::path exe(NTFS_BENCH_EXE);
  REQUIRE(std::filesystem::exists(exe));
  return Tests::RunSample(exe, args);
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

JsonValue Parse(const std::string& text) {
  std::string error;
  std::optional<JsonValue> parsed = NtfsDevice::ParseJson(text, error);
  INFO("parse error: " << error);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

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

std::wstring Option(std::wstring_view name, const std::filesystem::path& path) {
  return std::wstring(name) + L"=" + path.wstring();
}

// The simulated time of a run of `simulate` that has to succeed.
std::uint64_t SimulatedNs(const std::vector<std::wstring>& args) {
  std::vector<std::wstring> command{L"simulate"};
  command.insert(command.end(), args.begin(), args.end());
  const SampleRun run = RunBench(command);
  REQUIRE(run.exit_code == 0);
  return Count(Member(Parse(run.out), "sim_ns"));
}

// A plain file of zeros, in a temp folder.
std::filesystem::path WriteZeros(const Tests::TempDir& dir,
                                 std::string_view name, std::size_t bytes) {
  return dir.Write(name, std::string(bytes, '\0'));
}

// A run of NtfsMftList on an image, with its summary and read log, and the
// image the log was made from.
struct RecordedRun {
  Tests::TempDir dir;
  MftListSample::TempTreeImage image;
  std::filesystem::path summary_path;
  std::filesystem::path log_path;
  JsonValue summary;

  RecordedRun()
      : image(Tests::BuildFakeNtfsImageWithListMftScan(list_record_count,
                                                       list_fragment_clusters)),
        summary_path(dir.Path() / "summary.json"),
        log_path(dir.Path() / "summary.json.readlog") {
    const SampleRun run = MftListSample::RunMftList(
        {Option(L"--benchmark", summary_path), image.path.wstring()});
    REQUIRE(run.exit_code == 0);
    REQUIRE(std::filesystem::exists(log_path));
    summary = Parse(ReadText(summary_path));
  }
};

// The reads the classes of a run add up to, apart from the three that depend
// on whether the disk was quicker than the caller: io, window and ready.
struct ClassSplit {
  std::uint64_t unclassified;
  std::uint64_t announced;
  std::uint64_t plan;
};

ClassSplit Split(const JsonValue& classes) {
  const JsonValue& reads = Member(classes, "reads");
  return {.unclassified = Count(Member(reads, "unclassified")),
          .announced = Count(Member(reads, "io")) +
                       Count(Member(reads, "window")) +
                       Count(Member(reads, "ready")),
          .plan = Count(Member(reads, "plan"))};
}

const JsonValue& RunOfMode(const JsonValue& replay, std::string_view mode) {
  for (const JsonValue& run : Member(replay, "runs").items) {
    if (Member(run, "mode").text == mode) {
      return run;
    }
  }
  FAIL("no run of mode " << mode);
  return replay;
}

}  // namespace

TEST_CASE("NtfsBench simulate gives the same output twice",
          "[device-access][ntfsbench][sample]") {
  const std::string model = GENERATE(as<std::string>{}, "ssd", "hdd");
  INFO("model " << model);
  const std::vector<std::wstring> args{
      L"simulate", L"--records=500",
      L"--model=" + std::wstring(model.begin(), model.end())};

  const SampleRun first = RunBench(args);
  const SampleRun second = RunBench(args);

  REQUIRE(first.exit_code == 0);
  CHECK(first.out == second.out);
  CHECK(first.err == second.err);
  const JsonValue root = Parse(first.out);
  CHECK(Count(Member(root, "format")) == 1);
  CHECK(Member(root, "model").text == model);
  CHECK(Count(Member(root, "records")) == 500);
  CHECK(Count(Member(root, "entries")) > 0);
  CHECK(Count(Member(root, "sim_ns")) > 0);
  CHECK(Count(Member(root, "disk_reads")) > 0);
  CHECK(Count(Member(root, "disk_bytes")) > 0);
  CHECK(Count(Member(root, "served_reads")) +
            Count(Member(root, "unannounced_reads")) >
        0);
  CHECK(Count(Member(root, "unannounced_reads")) <=
        Count(Member(root, "disk_reads")));
  CHECK(Count(Member(root, "wait_ns")) <= Count(Member(root, "sim_ns")));
}

TEST_CASE("NtfsBench simulate: a hard disk takes longer than an SSD",
          "[device-access][ntfsbench][sample]") {
  CHECK(SimulatedNs({L"--model=hdd", L"--records=300"}) >
        SimulatedNs({L"--model=ssd", L"--records=300"}));
}

TEST_CASE("NtfsBench simulate: the model options move the time",
          "[device-access][ntfsbench][sample]") {
  const std::uint64_t base = SimulatedNs({L"--records=300"});

  CHECK(SimulatedNs({L"--records=300", L"--seek-us=5000"}) > base);
  CHECK(SimulatedNs({L"--records=300", L"--single-mbps=20"}) > base);
  CHECK(SimulatedNs({L"--records=300", L"--consume-ns-per-mib=5000000"}) >
        base);
}

TEST_CASE("NtfsBench simulate: a tuned constant moves the time",
          "[device-access][ntfsbench][sample]") {
  const std::uint64_t base = SimulatedNs({});

  const std::uint64_t starved = SimulatedNs({L"--tune=reader.max_reads=1"});

  CHECK(starved > base);
}

TEST_CASE("NtfsBench simulate writes its JSON to the file --json names",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::filesystem::path out = dir.Path() / "sim.json";

  const SampleRun to_stdout = RunBench({L"simulate", L"--records=200"});
  const SampleRun to_file =
      RunBench({L"simulate", L"--records=200", Option(L"--json", out)});

  REQUIRE(to_file.exit_code == 0);
  CHECK(to_file.out.empty());
  // The file keeps its line ends as they are; the captured text lost its CRs.
  std::string written = ReadText(out);
  std::erase(written, '\r');
  CHECK(written == to_stdout.out);
}

TEST_CASE("NtfsBench simulate --write-image writes a volume the tools open",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::filesystem::path image = dir.Path() / "volume.img";

  const SampleRun written = RunBench(
      {L"simulate", L"--records=400", Option(L"--write-image", image)});

  REQUIRE(written.exit_code == 0);
  CHECK(written.out.empty());
  REQUIRE(std::filesystem::exists(image));
  CHECK(std::filesystem::file_size(image) > 0);
  const SampleRun listed = MftListSample::RunMftList({image.wstring()});
  CHECK(listed.exit_code == 0);
  CHECK_FALSE(listed.out.empty());
}

TEST_CASE("NtfsBench knobs lists the tunable constants",
          "[device-access][ntfsbench][sample]") {
  const SampleRun run = RunBench({L"knobs"});

  REQUIRE(run.exit_code == 0);
  const JsonValue root = Parse(run.out);
  CHECK(Count(Member(root, "format")) == 1);
  const JsonValue& knobs = Member(root, "knobs");
  REQUIRE(knobs.items.size() == knob_count);
  CHECK(Tuning::Knobs().size() == knob_count);
  std::set<std::string> names;
  for (std::size_t index = 0; index < knobs.items.size(); index++) {
    const JsonValue& entry = knobs.items.at(index);
    const Tuning::Knob& knob = Tuning::Knobs()[index];
    INFO("knob " << knob.name);
    CHECK(Member(entry, "name").text == knob.name);
    CHECK(Member(entry, "family").text == Tuning::FamilyName(knob.family));
    CHECK(Count(Member(entry, "min")) == knob.min_value);
    CHECK(Count(Member(entry, "default")) == knob.default_value);
    CHECK(Count(Member(entry, "memory_unit_bytes")) == knob.memory_unit_bytes);
    CHECK(Member(entry, "per_pattern").boolean == knob.per_pattern);
    CHECK_FALSE(Member(entry, "patterns").items.empty());
    CHECK(Member(entry, "min").number <= Member(entry, "default").number);
    CHECK(Member(entry, "default").number <= Member(entry, "max").number);
    names.insert(Member(entry, "name").text);
  }
  CHECK(names.size() == knob_count);
}

TEST_CASE("NtfsBench replay gives the reads and the classes of the run",
          "[device-access][ntfsbench][sample]") {
  const RecordedRun recorded;
  const JsonValue& original = Member(recorded.summary, "run");
  const SampleRun run =
      RunBench({L"replay", recorded.log_path.wstring(),
                recorded.image.path.wstring(), L"--mode=both"});

  REQUIRE(run.exit_code == 0);
  const JsonValue replay = Parse(run.out);
  std::string error;
  const std::optional<Bench::ReadLog> log =
      Bench::LoadReadLog(recorded.log_path, &error);
  INFO(error);
  REQUIRE(log.has_value());
  CHECK(Count(Member(replay, "log_events")) == log->events.size());
  CHECK(Count(Member(replay, "log_volume_bytes")) == log->volume_bytes);
  CHECK(Member(replay, "runs").items.size() == 2);

  const JsonValue& as_recorded = RunOfMode(replay, "recorded");
  const JsonValue& originally = Member(original, "reads");
  CHECK(Count(Member(as_recorded, "reads")) ==
        Count(Member(originally, "requested")));
  CHECK(Count(Member(as_recorded, "read_bytes")) ==
        Count(Member(originally, "requested_bytes")));
  CHECK(Count(Member(as_recorded, "failed_reads")) == 0);
  CHECK(Count(Member(as_recorded, "wall_ns")) > 0);
  const auto count_events = [&log](Bench::EventType type) {
    return static_cast<std::uint64_t>(std::ranges::count_if(
        log->events,
        [type](const Bench::LogEvent& event) { return event.type == type; }));
  };
  CHECK(Count(Member(as_recorded, "prefetches")) ==
        count_events(Bench::EventType::Prefetch));
  CHECK(Count(Member(as_recorded, "cancels")) ==
        count_events(Bench::EventType::Cancel));

  // The classes of the replay are those of the run. How the announced reads
  // divide between io, window and ready depends on the speed of the disk on
  // that day; whether a read was announced does not.
  const ClassSplit first = Split(Member(original, "classes"));
  const ClassSplit again =
      Split(Member(Member(as_recorded, "report"), "classes"));
  CHECK(again.unclassified == first.unclassified);
  CHECK(again.announced == first.announced);
  CHECK(again.plan == first.plan);
  CHECK(again.unclassified + again.announced + again.plan ==
        Count(Member(as_recorded, "reads")));
  CHECK(Count(Member(Member(Member(as_recorded, "report"), "reads"),
                     "requested")) == Count(Member(as_recorded, "reads")));
}

TEST_CASE("NtfsBench replay with every read announced leaves none to plan",
          "[device-access][ntfsbench][sample]") {
  const RecordedRun recorded;

  const SampleRun run =
      RunBench({L"replay", recorded.log_path.wstring(),
                recorded.image.path.wstring(), L"--mode=ideal"});

  REQUIRE(run.exit_code == 0);
  const JsonValue replay = Parse(run.out);
  REQUIRE(Member(replay, "runs").items.size() == 1);
  const JsonValue& ideal = RunOfMode(replay, "ideal");
  CHECK(Count(Member(ideal, "cancels")) == 0);
  CHECK(Count(Member(ideal, "prefetches")) == Count(Member(ideal, "reads")));
  CHECK(Split(Member(Member(ideal, "report"), "classes")).plan == 0);
  CHECK(Count(Member(ideal, "failed_reads")) == 0);
}

TEST_CASE("NtfsBench replay applies --tune and says so",
          "[device-access][ntfsbench][sample]") {
  const RecordedRun recorded;

  const SampleRun run = RunBench(
      {L"replay", recorded.log_path.wstring(), recorded.image.path.wstring(),
       L"--mode=recorded", L"--tune=reader.max_reads=4"});

  REQUIRE(run.exit_code == 0);
  const JsonValue replay = Parse(run.out);
  const JsonValue& tuning = Member(replay, "tuning");
  for (const std::string_view pattern : {"M1", "M2", "M3", "M4"}) {
    INFO("pattern " << pattern);
    CHECK(Count(Member(Member(tuning, "reader.max_reads"), pattern)) == 4);
  }
}

TEST_CASE("NtfsBench replay refuses what it cannot use",
          "[device-access][ntfsbench][sample]") {
  const RecordedRun recorded;
  const std::wstring log = recorded.log_path.wstring();
  const std::wstring image = recorded.image.path.wstring();
  const Tests::TempDir dir;
  const std::filesystem::path not_a_log = dir.Write("not-a-log.bin", "hello");

  struct Case {
    std::vector<std::wstring> args;
    int exit_code;
    std::string message;
  };

  const std::vector<Case> cases{
      {{L"replay"}, 2, "replay expects <readlog> <target>"},
      {{L"replay", log}, 2, "replay expects <readlog> <target>"},
      {{L"replay", log, image, L"extra"}, 2, "replay expects"},
      {{L"replay", (dir.Path() / "absent.readlog").wstring(), image}, 1, ""},
      {{L"replay", not_a_log.wstring(), image}, 1, ""},
      {{L"replay", log, image, L"--mode=sideways"},
       2,
       "--mode is recorded, ideal or both"},
      {{L"replay", log, image, L"--tune=reader.nonsense=1"},
       2,
       "reader.nonsense"},
      {{L"replay", log, image, L"--tune=reader.max_reads=0"},
       2,
       "reader.max_reads"},
      {{L"replay", log, image, L"--tune=novalue"},
       2,
       "--tune expects name=value"},
      {{L"replay", log, (dir.Path() / "absent.img").wstring()},
       1,
       "Cannot open"},
  };
  for (const Case& test : cases) {
    INFO("arguments " << test.args.size() << ", expected exit "
                      << test.exit_code << ", message '" << test.message
                      << "'");
    const SampleRun run = RunBench(test.args);
    CHECK(run.exit_code == test.exit_code);
    CHECK(run.out.empty());
    CHECK_FALSE(run.err.empty());
    CHECK_THAT(run.err, ContainsSubstring(test.message));
  }
}

TEST_CASE("NtfsBench refuses a command line it does not understand",
          "[device-access][ntfsbench][sample]") {
  struct Case {
    std::vector<std::wstring> args;
    int exit_code;
    std::string message;
  };

  const std::vector<Case> cases{
      {{L"simulate", L"--records=10"}, 2, "at least 17"},
      {{L"simulate", L"--records=abc"}, 2, "--records expects a whole number"},
      {{L"simulate", L"--records="}, 2, "--records expects a whole number"},
      {{L"simulate", L"--model=floppy"}, 2, "--model is ssd or hdd"},
      {{L"simulate", L"--lanes=many"}, 2, "--lanes expects a whole number"},
      {{L"simulate", L"--seek-us=-1"}, 2, "--seek-us expects a whole number"},
      {{L"simulate", L"--tune=nonsense=1"}, 2, "nonsense"},
      {{L"simulate", L"--tune=reader.max_reads=999999"}, 2, "reader.max_reads"},
      {{L"simulate", L"--profile=absent-profile.json"}, 2, ""},
  };
  for (const Case& test : cases) {
    INFO("arguments " << test.args.size() << ", message '" << test.message
                      << "'");
    const SampleRun run = RunBench(test.args);
    CHECK(run.exit_code == test.exit_code);
    CHECK(run.out.empty());
    CHECK_THAT(run.err, ContainsSubstring(test.message));
    CHECK_FALSE(run.err.empty());
  }
}

TEST_CASE(
    "NtfsBench without a command, or with an unknown one, prints its usage",
    "[device-access][ntfsbench][sample]") {
  const SampleRun none = RunBench({});
  const SampleRun unknown = RunBench({L"defragment", L"c:"});

  CHECK(none.exit_code == 2);
  CHECK(unknown.exit_code == 2);
  for (const SampleRun* const run : {&none, &unknown}) {
    for (const std::string_view command :
         {"replay", "diskbench", "simulate", "knobs", "disk-id", "purge",
          "wake", "sample"}) {
      INFO("command " << command);
      CHECK_THAT(run->out, ContainsSubstring(std::string(command)));
    }
  }
}

TEST_CASE("NtfsBench commands that need a target refuse a missing one",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::wstring absent_file = (dir.Path() / "absent.img").wstring();
  const std::wstring absent(absent_target);

  struct Case {
    std::vector<std::wstring> args;
    int exit_code;
    std::string message;
  };

  const std::vector<Case> cases{
      {{L"diskbench"}, 2, "diskbench expects <target>"},
      {{L"diskbench", absent_file}, 1, "Cannot open"},
      {{L"disk-id"}, 2, "disk-id expects <target>"},
      {{L"wake"}, 2, "wake expects <target>"},
      {{L"sample"}, 2, "sample expects <target>"},
      {{L"sample", absent}, 2, "sample expects <target>"},
      {{L"sample", absent, L"--out=" + dir.Path().wstring() + L"/s.json"},
       2,
       "sample needs --pid, --seconds or --until-file"},
      {{L"sample", absent, L"--out=" + dir.Path().wstring() + L"/s.json",
        L"--seconds=lots"},
       2,
       "--seconds expects a whole number"},
      {{L"sample", absent, L"--out=" + dir.Path().wstring() + L"/s.json",
        L"--seconds=1"},
       1,
       "Cannot open"},
  };
  for (const Case& test : cases) {
    INFO("arguments " << test.args.size() << ", message '" << test.message
                      << "'");
    const SampleRun run = RunBench(test.args);
    CHECK(run.exit_code == test.exit_code);
    CHECK_THAT(run.err, ContainsSubstring(test.message));
  }
  CHECK_FALSE(std::filesystem::exists(dir.Path() / "s.json"));
}

TEST_CASE("NtfsBench disk-id refuses a target that does not exist",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::wstring missing_file = (dir.Path() / "absent.img").wstring();

  for (const std::wstring& target :
       {std::wstring(absent_target), missing_file}) {
    const SampleRun run = RunBench({L"disk-id", target});

    CHECK(run.exit_code == 1);
    CHECK(run.out.empty());
    CHECK_THAT(run.err, ContainsSubstring("Cannot open"));
  }
}

TEST_CASE("NtfsBench disk-id names no disk for a file",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::filesystem::path image = WriteZeros(dir, "disk.img", 4096);

  const SampleRun run = RunBench({L"disk-id", image.wstring()});

  REQUIRE(run.exit_code == 0);
  const JsonValue root = Parse(run.out);
  CHECK(Count(Member(root, "format")) == 1);
  CHECK_FALSE(Member(root, "found").boolean);
  CHECK(root.Find("key") == nullptr);
  CHECK_THAT(Member(root, "reason").text, ContainsSubstring("a file names no"));
}

TEST_CASE("NtfsBench wake refuses what is no disk and acts on no other drive",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::filesystem::path image = WriteZeros(dir, "disk.img", 4096);

  // The temp folder sits under a drive letter on Windows: neither path is
  // read as that drive.
  const SampleRun file = RunBench({L"wake", image.wstring()});
  const SampleRun missing =
      RunBench({L"wake", (dir.Path() / "absent.img").wstring()});
  const SampleRun nothing = RunBench({L"wake", std::wstring(absent_target)});

  CHECK(file.exit_code == 2);
  CHECK_THAT(file.err, ContainsSubstring("is a file"));
  CHECK(missing.exit_code == 1);
  CHECK_THAT(missing.err, ContainsSubstring("Cannot open"));
  CHECK(nothing.exit_code == 1);
  CHECK_THAT(nothing.err, ContainsSubstring("Cannot open"));
}

TEST_CASE("NtfsBench sample refuses a file and a missing path",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::filesystem::path image = WriteZeros(dir, "disk.img", 4096);
  const std::filesystem::path out = dir.Path() / "samples.json";

  const SampleRun file = RunBench(
      {L"sample", image.wstring(), Option(L"--out", out), L"--seconds=1"});
  const SampleRun missing =
      RunBench({L"sample", (dir.Path() / "absent.img").wstring(),
                Option(L"--out", out), L"--seconds=1"});

  CHECK(file.exit_code == 2);
  CHECK_THAT(file.err, ContainsSubstring("is a file"));
  CHECK(missing.exit_code == 1);
  CHECK_THAT(missing.err, ContainsSubstring("Cannot open"));
  CHECK_FALSE(std::filesystem::exists(out));
}

TEST_CASE("NtfsBench diskbench refuses a zone it cannot use",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::filesystem::path file = WriteZeros(dir, "disk.img", 1U << 20U);

  const SampleRun malformed =
      RunBench({L"diskbench", file.wstring(), L"--zone=somewhere"});
  const SampleRun tiny_zone =
      RunBench({L"diskbench", file.wstring(), L"--zone=0:100"});
  const SampleRun short_image = RunBench({L"diskbench", file.wstring()});
  const SampleRun seconds =
      RunBench({L"diskbench", file.wstring(), L"--seconds=soon"});

  CHECK(malformed.exit_code == 2);
  CHECK_THAT(malformed.err,
             ContainsSubstring("--zone is mft or offset:length"));
  CHECK(tiny_zone.exit_code == 1);
  CHECK_THAT(tiny_zone.err,
             ContainsSubstring("The zone is too small to measure"));
  // A file of 1 MiB holds no zone of two sequential reads.
  CHECK(short_image.exit_code == 1);
  CHECK_THAT(short_image.err, ContainsSubstring("zone is too small"));
  CHECK(seconds.exit_code == 2);
  CHECK_THAT(seconds.err,
             ContainsSubstring("--seconds expects a whole number"));
}

TEST_CASE("NtfsBench diskbench measures a plain file with no device",
          "[device-access][ntfsbench][sample]") {
  const Tests::TempDir dir;
  const std::filesystem::path file =
      WriteZeros(dir, "disk.img", measured_file_bytes);
  const std::filesystem::path out = dir.Path() / "sheet.json";

  const SampleRun run =
      RunBench({L"diskbench", file.wstring(),
                L"--zone=0:" + std::to_wstring(measured_file_bytes),
                L"--seconds=0", Option(L"--json", out)});

  REQUIRE(run.exit_code == 0);
  const JsonValue root = Parse(ReadText(out));
  CHECK(Count(Member(root, "format")) == 1);
  CHECK(Member(root, "target").text == file.string());
  const JsonValue& zone = Member(root, "zone");
  CHECK(Count(Member(zone, "offset")) == 0);
  CHECK(Count(Member(zone, "length")) == measured_file_bytes);
  const JsonValue& results = Member(root, "results");
  REQUIRE(results.items.size() == sheet_results);
  std::set<std::string> patterns;
  std::set<std::uint64_t> depths;
  for (const JsonValue& result : results.items) {
    patterns.insert(Member(result, "pattern").text);
    depths.insert(Count(Member(result, "queue_depth")));
    CHECK(Member(result, "mb_per_s").number >= 0);
  }
  CHECK(patterns == std::set<std::string>{"seq4m", "rand4k", "rand64k"});
  CHECK(depths == std::set<std::uint64_t>{1, 2, 4, 8, 16, 32});
}
