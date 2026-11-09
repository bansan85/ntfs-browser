#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <ntfs-browser/example/ntfsscanbench/via-scan.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/json.h>
#include <ntfs-browser/types.h>

#include "arguments.h"
#include "bench/bench.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// The engine of the scan that an engine of the tool names.
NTFS_BROWSER_NAMESPACE::EngineKind KindOf(Engine engine) {
  switch (engine) {
    case Engine::MftList:
      return NTFS_BROWSER_NAMESPACE::EngineKind::MftList;
    case Engine::NoCache:
      return NTFS_BROWSER_NAMESPACE::EngineKind::DirectoryNoCache;
    case Engine::FullCache:
      return NTFS_BROWSER_NAMESPACE::EngineKind::DirectoryFullCache;
    case Engine::Auto:
    case Engine::All:
      break;
  }
  return NTFS_BROWSER_NAMESPACE::EngineKind::Auto;
}

}  // namespace

NTFS_BROWSER_NAMESPACE::Options ScanOptionsOf(const Arguments& args,
                                              const RunPlan& plan) {
  NTFS_BROWSER_NAMESPACE::Options options;
  options.target = args.target_units;
  options.engine = KindOf(plan.engine);
  options.recursive = plan.recursive;
  options.include_deleted = args.volume_options.include_deleted;
  options.recover_errors = args.volume_options.recover_errors;
  options.pattern = args.pattern;
  options.filter = args.filter;
  return options;
}

int RunViaScan(const Arguments& args, const RunPlan& plan, Outcome& outcome) {
  ScanResult result;
  result.measure = MeasureScan(ScanOptionsOf(args, plan));
  outcome.entries = result.measure.entries;
  outcome.scan = result;
  if (result.measure.state != NTFS_BROWSER_NAMESPACE::State::Finished) {
    fprintf(stderr, "The scan did not finish\n");
    return -1;
  }
  return 0;
}

void WriteScanJson(Tool::DeviceAccess::JsonWriter& json,
                   const ScanResult& scan) {
  const auto number = [&](const char* key, double value) {
    json.Key(key);
    json.Number(value);
  };
  json.BeginObject();
  json.Field("entries", scan.measure.entries);
  json.Field("wall_ns", scan.measure.wall_ns);
  json.Key("worker");
  json.BeginObject();
  json.Field("wall_ns", scan.measure.worker.wall_ns);
  json.Field("cpu_ns", scan.measure.worker.cpu_ns);
  json.Field("wait_ns", scan.measure.worker.wait_ns);
  json.Field("match_ns", scan.measure.worker.match_ns);
  json.Field("exchange_ns", scan.measure.worker.exchange_ns);
  json.Field("entries_seen", scan.measure.worker.entries_seen);
  json.Field("entries_delivered", scan.measure.worker.entries_delivered);
  json.EndObject();
  json.Key("figures");
  json.BeginObject();
  json.Field("wall_ns", scan.figures.wall_ns);
  json.Field("io_ns", scan.figures.io_ns);
  json.Field("cpu_ns", scan.figures.cpu_ns);
  json.Field("total_cpu_ns", scan.figures.total_cpu_ns);
  json.Field("caller_cpu_ns", scan.figures.caller_cpu_ns);
  json.Field("worker_cpu_ns", scan.figures.worker_cpu_ns);
  json.Field("threads", static_cast<std::uint64_t>(scan.figures.threads));
  json.EndObject();
  json.Key("verdict");
  json.BeginObject();
  json.Field("critical_path", CriticalPathName(scan.verdict.path));
  number("io_share", scan.verdict.io_share);
  if (scan.verdict.overlap) {
    number("overlap_r", *scan.verdict.overlap);
  }
  number("cores_busy", scan.verdict.cores_busy);
  number("parallel_efficiency", scan.verdict.parallel_efficiency);
  number("worker_cpu_share", scan.verdict.worker_cpu_share);
  number("worker_cpu_of_total", scan.verdict.worker_cpu_of_total);
  json.EndObject();
  json.EndObject();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
