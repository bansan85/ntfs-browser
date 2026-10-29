#pragma once

#include <memory>
#include <string>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/volume-options.h>

#include "bench-options.h"
#include "json.h"
#include "sparse-capture.h"

namespace NtfsDevice {

// Writes report as the "run" members of an object the caller opened: the
// phases, the threads, the reads by kind and class, and the disk's side. The
// layout is documented in the README (the benchmark summary).
void WriteReportJson(JsonWriter& json,
                     const NTFS_BROWSER_NAMESPACE::Bench::Report& report);

// Writes the tunable constants of options as a "tuning" member: one value for
// each constant, an object by pattern for the reader's caps.
void WriteTuningJson(JsonWriter& json,
                     const NTFS_BROWSER_NAMESPACE::VolumeOptions& options);

// The benchmark of one run of a console tool. Built before the volume opens,
// so that the reads of the open are timed; Finish() ends it and writes what
// the command line asked for. Does nothing when it asked for nothing.
class BenchSession {
 public:
  // tool names the program, arguments are its command line as UTF-8, target is
  // what it was given to list, and options are the volume options in force.
  BenchSession(std::string tool, std::vector<std::string> arguments,
               std::string target, const BenchOptions& bench,
               const AppliedTuning& applied,
               const NTFS_BROWSER_NAMESPACE::VolumeOptions& options);
  BenchSession(const BenchSession&) = delete;
  BenchSession& operator=(const BenchSession&) = delete;
  BenchSession(BenchSession&&) = delete;
  BenchSession& operator=(BenchSession&&) = delete;
  // Finishes the session if the tool did not.
  ~BenchSession();

  // Whether a session runs: the run is timed or captured.
  [[nodiscard]] bool Active() const noexcept { return active_; }

  // Ends the session and writes the summary, the read log and the capture
  // size. Reports a failure on stderr. Safe to call twice.
  void Finish();

 private:
  std::string tool_;
  std::vector<std::string> arguments_;
  std::string target_;
  BenchOptions bench_;
  AppliedTuning applied_;
  NTFS_BROWSER_NAMESPACE::VolumeOptions options_;
  std::shared_ptr<SparseCapture> capture_;
  bool active_{false};
};

}  // namespace NtfsDevice
