#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/tuning/tuning.h>

#include "bench-session.h"
#include "bench-tool.h"
#include "json.h"
#include "text-convert.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;

namespace NtfsBenchTool {

namespace {

// One replay of the log, from a fresh reader: the reader's read ahead starts
// empty, as the tool's did.
struct Run {
  std::string mode;
  Bench::ReplayResult result;
  Bench::Report report;
};

}  // namespace

int ReplayCommand(const Arguments& args) {
  if (args.words.size() != 2) {
    std::fprintf(stderr, "replay expects <readlog> <target>\n");
    return 2;
  }
  const std::wstring& target = args.words.at(1);
  std::string error;
  const std::optional<Bench::ReadLog> log =
      Bench::LoadReadLog(NtfsDevice::PathFromWide(args.words.at(0)), &error);
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

  // The ideal plan keeps half of the reader's range cap announced: the reader
  // drops a hint past the cap, and the reads would then be unannounced again.
  std::size_t ahead = options->tuning.reader.front().max_announced_ranges;
  for (const Tuning::ReaderTuning& caps : options->tuning.reader) {
    ahead = (std::min)(ahead, caps.max_announced_ranges);
  }
  ahead = (std::max<std::size_t>)(1, ahead / 2);

  std::vector<Run> runs;
  for (const bool ideal : {false, true}) {
    if ((ideal && mode == "recorded") || (!ideal && mode == "ideal")) {
      continue;
    }
    if (args.Has("purge")) {
      std::string message;
      if (!PurgeSystemCache(message)) {
        std::fprintf(stderr, "%s\n", message.c_str());
        return 1;
      }
    }
    const std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader =
        OpenReader(target, options->tuning.reader);
    if (!reader) {
      return 1;
    }
    Bench::SessionOptions session;
    session.keep_log = false;
    Bench::Start(session);
    Run run;
    run.mode = ideal ? "ideal" : "recorded";
    run.result = Bench::Replay(*reader, *log,
                               {.mode = ideal ? Bench::ReplayMode::AllAnnounced
                                              : Bench::ReplayMode::AsRecorded,
                                .announce_ahead = ahead});
    run.report = Bench::Stop();
    runs.push_back(std::move(run));
  }

  NtfsDevice::JsonWriter json;
  json.BeginObject();
  json.Field("format", std::uint64_t{1});
  json.Field("command", "replay");
  json.Field("target", NtfsDevice::WideToUtf8(target));
  json.Field("log", NtfsDevice::WideToUtf8(args.words.at(0)));
  json.Field("log_events", static_cast<std::uint64_t>(log->events.size()));
  json.Field("log_volume_bytes", log->volume_bytes);
  json.Key("tuning");
  NtfsDevice::WriteTuningJson(json, *options);
  json.Key("runs");
  json.BeginArray();
  for (const Run& run : runs) {
    json.BeginObject();
    json.Field("mode", run.mode);
    json.Field("wall_ns", run.result.wall_ns);
    json.Field("reads", run.result.reads);
    json.Field("read_bytes", run.result.read_bytes);
    json.Field("failed_reads", run.result.failed_reads);
    json.Field("prefetches", run.result.prefetches);
    json.Field("cancels", run.result.cancels);
    json.Key("report");
    json.BeginObject();
    NtfsDevice::WriteReportJson(json, run.report);
    json.EndObject();
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
  return EmitJson(args, json);
}

}  // namespace NtfsBenchTool
