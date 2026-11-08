#include "scan-bench.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/tool-support.h>

#include "arguments.h"
#include "comparison-table.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// A stream that discards what is written to it, for the listing of a run that
// shows none. Binary, as the listing ends its own lines.
class NullStream {
 public:
  NullStream() {
#ifdef _WIN32
    file_ = std::fopen("NUL", "wb");
#else
    file_ = std::fopen("/dev/null", "wb");
#endif
  }

  NullStream(const NullStream&) = delete;
  NullStream& operator=(const NullStream&) = delete;
  NullStream(NullStream&&) = delete;
  NullStream& operator=(NullStream&&) = delete;

  ~NullStream() {
    if (file_ != nullptr) {
      (void)std::fclose(file_);
    }
  }

  // The stream, or null when the system has no null device.
  [[nodiscard]] std::FILE* Get() const noexcept { return file_; }

 private:
  std::FILE* file_ = nullptr;
};

// path with the engine name before its extension: "run.json" gives
// "run-nocache.json". An empty path stays empty.
std::filesystem::path WithEngine(const std::filesystem::path& path,
                                 Engine engine) {
  if (path.empty()) {
    return path;
  }
  std::filesystem::path named = path;
  named.replace_extension();
  named += "-";
  named += EngineName(engine);
  named += path.extension();
  return named;
}

// Runs the one engine args names, with the listing on stdout unless --quiet
// says it shows none.
int RunSingle(const Arguments& args) {
  RunPlan plan;
  plan.engine = args.engine;
  plan.bench = args.bench;
  plan.recursive = args.recursive;
  std::optional<NullStream> discard;
  if (args.quiet) {
    discard.emplace();
    if (discard->Get() == nullptr) {
      fprintf(stderr, "Cannot open the null device for --quiet\n");
      return -1;
    }
    plan.sink = discard->Get();
  }
  return RunEngine(args, plan).exit_code;
}

// Runs the three engines one after the other on the target, each in a
// recursive listing that is not shown and in a benchmark session of its own,
// then prints the table. The summary and the capture of each engine go to the
// file names of the command line with the engine before the extension.
// Returns 0 when all three finished.
int RunAll(const Arguments& args) {
  const NullStream discard;
  if (discard.Get() == nullptr) {
    fprintf(stderr, "Cannot open the null device\n");
    return -1;
  }
  constexpr std::array<Engine, 3> engines{Engine::MftList, Engine::NoCache,
                                          Engine::FullCache};
  std::vector<EngineRow> rows;
  int exit_code = 0;
  for (const Engine engine : engines) {
    RunPlan plan;
    plan.engine = engine;
    plan.bench = args.bench;
    plan.bench.summary_path = WithEngine(args.bench.summary_path, engine);
    plan.bench.capture_path = WithEngine(args.bench.capture_path, engine);
    plan.sink = discard.Get();
    plan.recursive = true;
    plan.measure = true;
    plan.release = true;
    const Outcome outcome = RunEngine(args, plan);
    rows.push_back(MakeRow(engine, outcome));
    if (outcome.exit_code != 0) {
      exit_code = -1;
    }
  }
  const std::string table = FormatTable(rows);
  fwrite(table.data(), 1, table.size(), stdout);
  return exit_code;
}

}  // namespace

int Run(int argc, wchar_t* argv[]) {
  Arguments args;
  if (!ParseArguments(argc, argv, args)) {
    Usage();
    return -1;
  }
  if (!Log::Configure(args.log_config)) {
    Tool::DeviceAccess::ReportLogFileError(args.log_config);
  }

  // Names print as UTF-8: a console shows them right with its code page set.
  const Tool::DeviceAccess::Utf8ConsoleOutput console;
  return args.engine == Engine::All ? RunAll(args) : RunSingle(args);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
