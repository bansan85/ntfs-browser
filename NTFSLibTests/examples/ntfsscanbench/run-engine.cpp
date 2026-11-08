#include <cstdio>
#include <string>
#include <utility>

#include <ntfs-browser/example/ntfsscanbench/via-scan.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/tool/device-access/bench-session.h>
#include <ntfs-browser/tool/device-access/json.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/volume-options.h>

#include "arguments.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// Ends the session of a run through the scan and finishes what only the end can
// give: the process CPU and the thread count from the report, T_io from the
// read log, T_cpu from a second scan whose reads come from the file cache, and
// the verdicts. The summary gets the result as its "scan" member.
void CompleteScanResult(const Arguments& args, const RunPlan& plan,
                        const VolumeOptions& volume_options,
                        Tool::DeviceAccess::BenchSession& session,
                        Outcome& outcome) {
  session.Collect();
  if (!outcome.scan || !session.Report()) {
    return;
  }
  ScanResult& scan = *outcome.scan;
  scan.figures = FiguresOf(*session.Report(), scan.measure);
  if (scan.measure.state == Search::State::Finished) {
    if (const auto& log = session.KeptReadLog()) {
      scan.figures.io_ns =
          ReplayNs(args.target, *log, volume_options.tuning.reader).value_or(0);
    }
    // The image is in the file cache now: this run costs the CPU alone.
    scan.figures.cpu_ns = MeasureScan(ScanOptionsOf(args, plan)).wall_ns;
  }
  scan.verdict = Judge(scan.figures);
  session.SetExtra("scan", [&scan](Tool::DeviceAccess::JsonWriter& json) {
    WriteScanJson(json, scan);
  });
}

}  // namespace

Outcome RunEngine(const Arguments& args, const RunPlan& plan) {
  Outcome outcome;

  // The profile and --tune settings, before the volume opens: the reader takes
  // its caps then.
  VolumeOptions volume_options = args.volume_options;
  Tool::DeviceAccess::AppliedTuning applied;
  if (const std::string failure = Tool::DeviceAccess::ApplyTuning(
          plan.bench, args.target, volume_options, applied);
      !failure.empty()) {
    fprintf(stderr, "%s\n", failure.c_str());
    outcome.exit_code = -1;
    return outcome;
  }
  if (!plan.bench.profile_path.empty() && !applied.section_found) {
    fprintf(stderr,
            "The profile has no section for this disk: defaults used\n");
  }

  {
    Tool::DeviceAccess::BenchSession session(
        {.tool = "NtfsScanBench",
         .arguments = args.command_line,
         .target = Tool::DeviceAccess::WideToUtf8(args.target),
         .engine = std::string(EngineName(plan.engine)),
         .measure = plan.measure || plan.via_scan,
         .keep_read_log = plan.via_scan},
        plan.bench, applied, volume_options);

    if (plan.via_scan) {
      outcome.exit_code = RunViaScan(args, plan, outcome);
      CompleteScanResult(args, plan, volume_options, session, outcome);
    } else {
      outcome.exit_code =
          plan.engine == Engine::MftList
              ? RunMftListEngine(args, volume_options, plan, outcome.entries)
              : RunWalkEngine(args, volume_options, plan, outcome.entries);
    }

    session.Finish();
    outcome.report = session.Report();
  }
  return outcome;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
