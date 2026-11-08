#include <cstdio>
#include <string>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/tool/device-access/bench-session.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/volume-options.h>

#include "arguments.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

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
         .measure = plan.measure},
        plan.bench, applied, volume_options);

    outcome.exit_code =
        plan.engine == Engine::MftList
            ? RunMftListEngine(args, volume_options, plan, outcome.entries)
            : RunWalkEngine(args, volume_options, plan, outcome.entries);

    session.Finish();
    outcome.report = session.Report();
  }
  return outcome;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
