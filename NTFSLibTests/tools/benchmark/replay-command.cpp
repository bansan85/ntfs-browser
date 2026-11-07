#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-session.h>
#include <ntfs-browser/tool/device-access/json.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "bench-tool.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;

namespace NTFS_BROWSER_NAMESPACE::Tool::Benchmark {

namespace {

// One replay of the log, from a fresh reader: the reader's read ahead starts
// empty, as the tool's did.
struct Run {
  std::string mode;
  Bench::ReplayResult result;
  Bench::Report report;
};

// How many ranges the ideal plan keeps announced ahead. It keeps half of the
// reader's range cap: the reader drops a hint past the cap, and the reads would
// then be unannounced again.
std::size_t IdealAnnounceAhead(const Tuning::Settings& tuning) {
  std::size_t ahead = tuning.reader.front().max_announced_ranges;
  for (const Tuning::ReaderTuning& caps : tuning.reader) {
    ahead = (std::min)(ahead, caps.max_announced_ranges);
  }
  return (std::max<std::size_t>)(1, ahead / 2);
}

// Replays the log once, from a fresh reader. Nothing when the cache cannot be
// purged or the target cannot be opened; the reason is printed.
std::optional<Run> ReplayOnce(const Arguments& args, const Bench::ReadLog& log,
                              const std::wstring& target,
                              const Tuning::Settings& tuning, bool ideal) {
  if (args.Has("purge")) {
    std::string message;
    if (!PurgeSystemCache(message)) {
      std::fprintf(stderr, "%s\n", message.c_str());
      return std::nullopt;
    }
  }
  const std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader =
      OpenReader(target, tuning.reader);
  if (!reader) {
    return std::nullopt;
  }
  Bench::SessionOptions session;
  session.keep_log = false;
  Bench::Start(session);
  Run run;
  run.mode = ideal ? "ideal" : "recorded";
  run.result = Bench::Replay(*reader, log,
                             {.mode = ideal ? Bench::ReplayMode::AllAnnounced
                                            : Bench::ReplayMode::AsRecorded,
                              .announce_ahead = IdealAnnounceAhead(tuning)});
  run.report = Bench::Stop();
  return run;
}

// Writes the counts and the report of one replay.
void WriteRun(Tool::DeviceAccess::JsonWriter& json, const Run& run) {
  json.BeginObject();
  json.Field("mode", run.mode);
  json.Field("wall_ns", run.result.wall_ns);
  json.Field("reads", run.result.reads);
  json.Field("read_bytes", run.result.read_bytes);
  json.Field("failed_reads", run.result.failed_reads);
  json.Field("prefetches", run.result.prefetches);
  json.Field("cancels", run.result.cancels);
  json.Field("standing_prefetches", run.result.standing_prefetches);
  json.Field("standing_cancels", run.result.standing_cancels);
  json.Key("report");
  json.BeginObject();
  Tool::DeviceAccess::WriteReportJson(json, run.report);
  json.EndObject();
  json.EndObject();
}

}  // namespace

int ReplayCommand(const Arguments& args) {
  if (args.words.size() != 2) {
    std::fprintf(stderr, "replay expects <readlog> <target>\n");
    return 2;
  }
  const std::wstring& target = args.words.at(1);
  std::string error;
  const std::optional<Bench::ReadLog> log = Bench::LoadReadLog(
      Tool::DeviceAccess::PathFromWide(args.words.at(0)), &error);
  if (!log) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const auto options = TunedOptions(args, target);
  if (!options) {
    return 2;
  }
  const std::string mode = args.Value("mode").value_or("both");
  if (mode != "recorded" && mode != "ideal" && mode != "both") {
    std::fprintf(stderr, "--mode is recorded, ideal or both\n");
    return 2;
  }

  std::vector<Run> runs;
  for (const bool ideal : {false, true}) {
    if ((ideal && mode == "recorded") || (!ideal && mode == "ideal")) {
      continue;
    }
    std::optional<Run> run =
        ReplayOnce(args, *log, target, options->tuning, ideal);
    if (!run) {
      return 1;
    }
    runs.push_back(std::move(*run));
  }

  Tool::DeviceAccess::JsonWriter json;
  json.BeginObject();
  json.Field("format", std::uint64_t{1});
  json.Field("command", "replay");
  json.Field("target", Tool::DeviceAccess::WideToUtf8(target));
  json.Field("log", Tool::DeviceAccess::WideToUtf8(args.words.at(0)));
  json.Field("log_events", static_cast<std::uint64_t>(log->events.size()));
  json.Field("log_volume_bytes", log->volume_bytes);
  json.Key("tuning");
  Tool::DeviceAccess::WriteTuningJson(json, *options);
  json.Key("runs");
  json.BeginArray();
  for (const Run& run : runs) {
    WriteRun(json, run);
  }
  json.EndArray();
  json.EndObject();
  return EmitJson(args, json);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::Benchmark
