#include <cstdint>
#include <cstdio>
#include <string>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/dir-walk/directory-walk.h>
#include <ntfs-browser/volume-options.h>

#include "arguments.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

int RunWalkEngine(const Arguments& args, const VolumeOptions& volume_options,
                  const RunPlan& plan, std::uint64_t& entries) {
  // ListDirectory splits the path in place, so it gets a copy of the target.
  std::wstring path(args.target);
  Tool::DirWalk::Total total;
  const int exit_code =
      plan.engine == Engine::FullCache
          ? Tool::DirWalk::ListDirectory<Cache::Strategy::FullCache>(
                path.data(), plan.recursive, volume_options, plan.sink, &total)
          : Tool::DirWalk::ListDirectory<Cache::Strategy::NoCache>(
                path.data(), plan.recursive, volume_options, plan.sink, &total);
  entries = static_cast<std::uint64_t>(total.files) +
            static_cast<std::uint64_t>(total.dirs);
  return exit_code;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
