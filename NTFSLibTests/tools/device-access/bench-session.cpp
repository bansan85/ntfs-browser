#include "bench-session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/tuning/knobs.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "disk-identity.h"
#include "json.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;

namespace NtfsDevice {

namespace {

// Version of the layout of the summary. A change of it MUST bump this.
constexpr std::uint64_t summary_format = 1;

void WriteHistogram(JsonWriter& json, const Bench::Histogram& histogram) {
  json.BeginObject();
  json.Field("count", histogram.count);
  json.Field("sum", histogram.sum);
  json.Field("max", histogram.max);
  // Bucket i holds the values from 2^i up to 2^(i+1) - 1, bucket 0 also 0.
  json.Key("buckets");
  json.BeginArray();
  for (const std::uint64_t bucket : histogram.buckets) {
    json.Number(bucket);
  }
  json.EndArray();
  json.EndObject();
}

void WriteByClass(
    JsonWriter& json,
    const std::array<std::uint64_t, Bench::read_class_count>& counts) {
  json.BeginObject();
  for (std::size_t index = 0; index < Bench::read_class_count; index++) {
    json.Field(Bench::ReadClassName(static_cast<Bench::ReadClass>(index)),
               counts.at(index));
  }
  json.EndObject();
}

}  // namespace

void WriteReportJson(JsonWriter& json, const Bench::Report& report) {
  json.Field("start_unix_ns", report.start_unix_ns);
  json.Field("wall_ns", report.wall_ns);
  json.Field("volume_bytes", report.volume_bytes);
  json.Field("media_kind", MediaKindName(report.media_kind));

  json.Key("phases");
  json.BeginArray();
  for (const Bench::PhaseStat& phase : report.phases) {
    json.BeginObject();
    json.Field("name", Bench::PhaseName(phase.phase));
    json.Field("count", static_cast<std::uint64_t>(phase.count));
    json.Field("wall_ns", phase.wall_ns);
    json.Field("cpu_ns", phase.cpu_ns);
    json.Field("first_begin_ns", phase.first_begin_ns);
    json.Field("last_end_ns", phase.last_end_ns);
    json.Field("reads", phase.reads);
    json.Field("read_bytes", phase.read_bytes);
    json.Field("read_wait_ns", phase.read_wait_ns);
    json.EndObject();
  }
  json.EndArray();

  json.Key("threads");
  json.BeginArray();
  for (const Bench::ThreadStat& thread : report.threads) {
    json.BeginObject();
    json.Field("role", Bench::ThreadRoleName(thread.role));
    json.Field("user_ns", thread.user_ns);
    json.Field("kernel_ns", thread.kernel_ns);
    json.Field("wall_ns", thread.wall_ns);
    json.EndObject();
  }
  json.EndArray();

  json.Key("process_cpu");
  json.BeginObject();
  json.Field("user_ns", report.process_user_ns);
  json.Field("kernel_ns", report.process_kernel_ns);
  json.EndObject();

  // The main thread's time in each slice, without the time spent in a nested
  // slice, nor waiting for the disk.
  json.Key("main_thread_ns");
  json.BeginObject();
  for (std::size_t index = 0; index < Bench::slice_count; index++) {
    json.Field(Bench::SliceName(static_cast<Bench::Slice>(index)),
               report.slices_ns.at(index));
  }
  json.Field("read_wait", report.read_wait_ns);
  json.Field("serial_tail", report.tail_ns);
  json.EndObject();

  json.Key("reads");
  json.BeginObject();
  json.Field("requested", report.requested_reads);
  json.Field("requested_bytes", report.requested_bytes);
  json.Field("call_ns", report.read_call_ns);
  json.Field("wait_ns", report.read_wait_ns);
  json.Field("last_end_ns", report.last_read_end_ns);
  json.Field("tail_ns", report.tail_ns);
  json.Field("prefetch_calls", report.prefetch_calls);
  json.Field("prefetch_bytes", report.prefetch_bytes);
  json.Field("cancel_calls", report.cancel_calls);
  json.Field("standing_prefetch_calls", report.standing_prefetch_calls);
  json.Field("standing_prefetch_bytes", report.standing_prefetch_bytes);
  json.Field("standing_cancel_calls", report.standing_cancel_calls);
  json.Field("events_logged", report.events_logged);
  json.Field("events_dropped", report.events_dropped);
  json.EndObject();

  json.Key("classes");
  json.BeginObject();
  json.Key("reads");
  WriteByClass(json, report.class_reads);
  json.Key("wait_ns");
  WriteByClass(json, report.class_wait_ns);
  json.EndObject();

  json.Key("kinds");
  json.BeginObject();
  for (const Bench::KindStat& kind : report.kinds) {
    if (kind.reads == 0) {
      continue;
    }
    json.Key(Bench::ReadKindName(kind.kind));
    json.BeginObject();
    json.Field("reads", kind.reads);
    json.Field("bytes", kind.bytes);
    json.Field("wait_ns", kind.wait_ns);
    json.Key("sizes");
    WriteHistogram(json, kind.sizes);
    json.Key("classes");
    WriteByClass(json, kind.by_class);
    json.EndObject();
  }
  json.EndObject();

  json.Key("disk");
  json.BeginObject();
  const Bench::DiskStat& disk = report.disk;
  json.Field("reads", disk.reads);
  json.Field("bytes", disk.bytes);
  json.Field("prefetch_reads", disk.prefetch_reads);
  json.Field("prefetch_bytes", disk.prefetch_bytes);
  json.Field("wasted_bytes", disk.wasted_bytes);
  json.Field("standing_reads", disk.standing_reads);
  json.Field("standing_bytes", disk.standing_bytes);
  json.Field("standing_wasted_bytes", disk.standing_wasted_bytes);
  json.Field("seeks", disk.seeks);
  json.Field("seek_bytes", disk.seek_bytes);
  // Bytes the disk read for each byte the callers asked for.
  json.Key("amplification");
  json.Number(report.requested_bytes == 0
                  ? 0.0
                  : static_cast<double>(disk.bytes) /
                        static_cast<double>(report.requested_bytes));
  json.Key("sizes");
  WriteHistogram(json, disk.sizes);
  json.Key("queue_depth");
  json.BeginArray();
  for (const std::uint64_t reads : disk.queue_depth) {
    json.Number(reads);
  }
  json.EndArray();
  json.EndObject();
}

void WriteTuningJson(JsonWriter& json,
                     const NTFS_BROWSER_NAMESPACE::VolumeOptions& options) {
  json.BeginObject();
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    json.Key(knob.name);
    if (!knob.per_pattern) {
      json.Number(knob.get(options, Tuning::Pattern::LargeSequential));
      continue;
    }
    json.BeginObject();
    for (std::size_t index = 0; index < Tuning::pattern_count; index++) {
      const auto pattern = static_cast<Tuning::Pattern>(index);
      json.Field(Tuning::PatternName(pattern), knob.get(options, pattern));
    }
    json.EndObject();
  }
  json.EndObject();
}

BenchSession::BenchSession(std::string tool, std::vector<std::string> arguments,
                           std::string target, const BenchOptions& bench,
                           const AppliedTuning& applied,
                           const NTFS_BROWSER_NAMESPACE::VolumeOptions& options)
    : tool_(std::move(tool)),
      arguments_(std::move(arguments)),
      target_(std::move(target)),
      bench_(bench),
      applied_(applied),
      options_(options) {
  if (bench.stop_after) {
    Bench::SetStopAfter(bench.stop_after);
  }
  if (!bench.WantsSession()) {
    return;
  }
  if (!bench.capture_path.empty()) {
    capture_ = std::make_shared<SparseCapture>(bench.capture_path);
    if (!capture_->Ok()) {
      std::fprintf(stderr, "%s\n", capture_->Error().c_str());
      capture_.reset();
    } else {
      Bench::SetCaptureSink(capture_);
    }
  }
  Bench::SessionOptions session;
  // A capture run is not a timing run: it keeps no log.
  session.keep_log = bench.benchmark;
  Bench::Start(session);
  active_ = true;
}

BenchSession::~BenchSession() {
  try {
    Finish();
  } catch (...) {
    // A destructor cannot report; Finish() already printed what failed.
  }
}

void BenchSession::Finish() {
  if (!active_) {
    return;
  }
  active_ = false;
  const Bench::Report report = Bench::Stop();
  Bench::ReadLog log = Bench::TakeReadLog();
  Bench::SetCaptureSink(nullptr);

  if (capture_) {
    if (!capture_->SetSize(report.volume_bytes) || !capture_->Ok()) {
      std::fprintf(stderr, "Cannot finish the capture file: %s\n",
                   capture_->Error().c_str());
    }
    capture_.reset();
  }
  if (!bench_.benchmark) {
    return;
  }

  std::string error;
  std::filesystem::path log_path = bench_.summary_path;
  log_path += ".readlog";
  if (!Bench::WriteReadLog(log_path, log, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
  }

  JsonWriter json;
  json.BeginObject();
  json.Field("format", summary_format);
  json.Field("tool", tool_);
  json.Field("target", target_);
  json.Key("arguments");
  json.BeginArray();
  for (const std::string& argument : arguments_) {
    json.String(argument);
  }
  json.EndArray();
  json.Field("read_log", log_path.string());
  json.Field("stop_after", bench_.stop_after
                               ? Bench::PhaseName(*bench_.stop_after)
                               : std::string_view());

  json.Key("profile");
  json.BeginObject();
  json.Field("path", bench_.profile_path.string());
  json.Field("loaded", applied_.profile_loaded);
  json.Field("section_found", applied_.section_found);
  json.Field("disk", applied_.disk_key);
  json.Key("applied");
  json.BeginArray();
  for (const std::string& setting : applied_.applied) {
    json.String(setting);
  }
  json.EndArray();
  json.EndObject();

  json.Key("tuning");
  WriteTuningJson(json, options_);

  json.Key("run");
  json.BeginObject();
  WriteReportJson(json, report);
  json.EndObject();
  json.EndObject();

  std::ofstream out(
      NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(bench_.summary_path),
      std::ios::binary | std::ios::trunc);
  const std::string text = json.Take();
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    std::fprintf(stderr, "Cannot write %s\n",
                 bench_.summary_path.string().c_str());
    return;
  }
  std::fprintf(stderr, "Benchmark summary: %s\n",
               bench_.summary_path.string().c_str());
}

}  // namespace NtfsDevice
