#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/volume-options.h>

#include "arguments.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

// What one run of one engine needs besides the command line.
struct RunPlan {
  // The engine to run: never All.
  Engine engine = Engine::MftList;
  // The benchmark options of this run. They are the ones of the command line,
  // except in a run of all the engines, where the file names carry the engine.
  Tool::DeviceAccess::BenchOptions bench;
  // Where the listing goes: stdout, or a stream that discards it.
  std::FILE* sink = stdout;
  // Whether a walk lists every directory below the target.
  bool recursive = false;
  // Times the run even when the command line asks for no benchmark: the table
  // of a run of all the engines reads the numbers.
  bool measure = false;
  // Frees what the run built before it ends. A run that is the last of the
  // process leaves it to the system, which is faster; a run that others
  // follow gives the memory back.
  bool release = false;
};

// What one run gave.
struct Outcome {
  int exit_code = 0;
  // The entries the listing shows: the records of $MFT for the mftlist
  // engine, the files and directories for a walk.
  std::uint64_t entries = 0;
  // The numbers of the session, when the run had one.
  std::optional<NTFS_BROWSER_NAMESPACE::Bench::Report> report;
};

// Runs plan.engine on the target of args, in a benchmark session of its own.
// Applies the profile and the --tune settings first, as the reader takes its
// caps when the volume opens. Reports a bad profile on stderr and returns -1
// as the exit code.
Outcome RunEngine(const Arguments& args, const RunPlan& plan);

// Lists every record of $MFT of the volume args names, with MftTree. Writes
// the listing to plan.sink and counts the records in entries. Returns the exit
// code. volume_options are the options of args after the tuning.
int RunMftListEngine(
    const Arguments& args,
    const NTFS_BROWSER_NAMESPACE::VolumeOptions& volume_options,
    const RunPlan& plan, std::uint64_t& entries);

// Lists the directory args names, by walking the directory indexes with a
// volume of the cache strategy of plan.engine (NoCache or FullCache). Writes
// the listing to plan.sink and counts the files and directories in entries.
// Returns the exit code.
int RunWalkEngine(const Arguments& args,
                  const NTFS_BROWSER_NAMESPACE::VolumeOptions& volume_options,
                  const RunPlan& plan, std::uint64_t& entries);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
