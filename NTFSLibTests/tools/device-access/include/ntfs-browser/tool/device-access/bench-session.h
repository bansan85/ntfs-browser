#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>

#include "bench/bench.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

class JsonWriter;
class SparseCapture;

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
  // How the summary names a run.
  struct RunInfo {
    // The program.
    std::string tool;
    // Its command line, as UTF-8.
    std::vector<std::string> arguments;
    // What it was given to list.
    std::string target;
    // The engine of a tool that has several, named in the summary. Empty for
    // a tool with one.
    std::string engine;
    // Starts a session even when the command line asks for no benchmark and
    // no capture, so that Report() has the numbers of the run.
    bool measure{false};
    // Keeps the read log after Finish(), for ReadLog(), even when the command
    // line asks for no summary.
    bool keep_read_log{false};
  };

  // options are the volume options in force.
  BenchSession(RunInfo run, const BenchOptions& bench,
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

  // Ends the session and gathers its report and read log, without writing
  // anything. A tool that has to compute from them before the summary is
  // written calls it, then SetExtra(), then Finish(). Safe to call twice.
  void Collect();

  // Adds the member name to the summary, filled by writer: the writer writes
  // one JSON value. Call it before Finish().
  void SetExtra(std::string name, std::function<void(JsonWriter&)> writer);

  // Ends the session if Collect() did not, and writes the summary, the read log
  // and the capture size. Reports a failure on stderr. Safe to call twice.
  void Finish();

  // The read log of the run, once Collect() or Finish() has ended a session,
  // when RunInfo::keep_read_log asked for it.
  [[nodiscard]] const std::optional<NTFS_BROWSER_NAMESPACE::Bench::ReadLog>&
      KeptReadLog() const noexcept {
    return kept_log_;
  }

  // The numbers of the run, once Finish() has ended a session. Empty before,
  // and for a run that had no session.
  [[nodiscard]] const std::optional<NTFS_BROWSER_NAMESPACE::Bench::Report>&
      Report() const noexcept {
    return report_;
  }

 private:
  // The summary of a finished run, as JSON.
  [[nodiscard]] std::string
      BuildSummary(const NTFS_BROWSER_NAMESPACE::Bench::Report& report,
                   const std::filesystem::path& log_path) const;

  // The read log that Collect() took, until Finish() writes it.
  std::optional<NTFS_BROWSER_NAMESPACE::Bench::ReadLog> pending_log_;
  std::optional<NTFS_BROWSER_NAMESPACE::Bench::ReadLog> kept_log_;
  std::string extra_name_;
  std::function<void(JsonWriter&)> extra_writer_;
  bool finished_{false};

  RunInfo run_;
  BenchOptions bench_;
  AppliedTuning applied_;
  NTFS_BROWSER_NAMESPACE::VolumeOptions options_;
  std::shared_ptr<SparseCapture> capture_;
  std::optional<NTFS_BROWSER_NAMESPACE::Bench::Report> report_;
  bool active_{false};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
