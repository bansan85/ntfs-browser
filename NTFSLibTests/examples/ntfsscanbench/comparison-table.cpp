#include "comparison-table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#include <ntfs-browser/namespace.h>

#include "arguments.h"
#include "bench/bench.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// Nanoseconds in a millisecond, for the time columns.
constexpr double ns_per_ms = 1e6;

// Room for one formatted line of the scan tables, newline included. The widest
// row has 9 columns of at most 12 characters.
constexpr size_t scan_line_size = 200;

// Nanoseconds as milliseconds, for the time columns.
double Ms(std::uint64_t ns) { return static_cast<double>(ns) / ns_per_ms; }

// The name of the engine of a row, as printed in the first column.
std::string Name(const EngineRow& row) {
  return std::string(EngineName(row.engine));
}

// Formats one line with snprintf, then appends it to a table. A line that does
// not fit is cut, never overrun.
class LineWriter {
 public:
  explicit LineWriter(std::string& table) : table_(table) {}

  // The buffer for the next line. Pass the length that snprintf returns to
  // Add().
  char* Buffer() { return line_.data(); }

  size_t Size() const { return line_.size(); }

  // Appends the line that snprintf wrote into Buffer().
  void Add(int length) {
    if (length > 0) {
      table_.append(line_.data(),
                    (std::min)(static_cast<size_t>(length), line_.size() - 1));
    }
  }

  // Appends text that needs no formatting.
  void AddText(const char* text) { table_ += text; }

 private:
  std::string& table_;
  std::array<char, scan_line_size> line_{};
};

// Appends the table of the verdict: where the time of each scan went.
void FormatVerdictTable(std::span<const EngineRow> rows, std::string& tables) {
  LineWriter out(tables);
  out.AddText(
      "\nVerdicts (T_io: read log replayed. T_cpu: a second, warm run)\n");
  out.Add(snprintf(out.Buffer(), out.Size(),
                   "%-10s %10s %10s %10s %8s %8s %-10s\n", "Engine",
                   "Real (ms)", "IO (ms)", "CPU (ms)", "IO/Real", "R", "Path"));
  for (const EngineRow& row : rows) {
    if (!row.scan) {
      continue;
    }
    const ScanFigures& figures = row.scan->figures;
    const Verdict& verdict = row.scan->verdict;
    out.Add(snprintf(out.Buffer(), out.Size(),
                     "%-10s %10.1f %10.1f %10.1f %8.2f %8.2f %-10s\n",
                     Name(row).c_str(), Ms(figures.wall_ns), Ms(figures.io_ns),
                     Ms(figures.cpu_ns), verdict.io_share,
                     verdict.overlap.value_or(0.0),
                     std::string(CriticalPathName(verdict.path)).c_str()));
  }
}

// Appends the table of the worker thread of each scan.
void FormatWorkerTable(std::span<const EngineRow> rows, std::string& tables) {
  LineWriter out(tables);
  out.AddText("\nWorker thread of the scan (ms)\n");
  out.Add(snprintf(out.Buffer(), out.Size(),
                   "%-10s %10s %10s %10s %10s %10s %10s %12s %12s\n", "Engine",
                   "Wall", "CPU", "Wait", "Match", "Exchange", "Caller CPU",
                   "Seen", "Delivered"));
  for (const EngineRow& row : rows) {
    if (!row.scan) {
      continue;
    }
    const WorkerFigures& worker = row.scan->measure.worker;
    out.Add(snprintf(
        out.Buffer(), out.Size(),
        "%-10s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %12llu %12llu\n",
        Name(row).c_str(), Ms(worker.wall_ns), Ms(worker.cpu_ns),
        Ms(worker.wait_ns), Ms(worker.match_ns), Ms(worker.exchange_ns),
        Ms(row.scan->figures.caller_cpu_ns),
        static_cast<unsigned long long>(worker.entries_seen),
        static_cast<unsigned long long>(worker.entries_delivered)));
  }
}

// Appends the table of the threads and cores each scan used.
void FormatParallelismTable(std::span<const EngineRow> rows,
                            std::string& tables) {
  LineWriter out(tables);
  out.AddText("\nParallelism\n");
  out.Add(snprintf(out.Buffer(), out.Size(),
                   "%-10s %12s %8s %8s %10s %12s %12s\n", "Engine", "CPU (ms)",
                   "Threads", "Cores", "Efficiency", "Worker/Real",
                   "Worker/CPU"));
  for (const EngineRow& row : rows) {
    if (!row.scan) {
      continue;
    }
    const ScanFigures& figures = row.scan->figures;
    const Verdict& verdict = row.scan->verdict;
    out.Add(snprintf(out.Buffer(), out.Size(),
                     "%-10s %12.1f %8u %8.2f %10.2f %12.2f %12.2f\n",
                     Name(row).c_str(), Ms(figures.total_cpu_ns),
                     figures.threads, verdict.cores_busy,
                     verdict.parallel_efficiency, verdict.worker_cpu_share,
                     verdict.worker_cpu_of_total));
  }
}

}  // namespace

EngineRow MakeRow(Engine engine, const Outcome& outcome) {
  EngineRow row;
  row.engine = engine;
  row.exit_code = outcome.exit_code;
  row.entries = outcome.entries;
  row.scan = outcome.scan;
  if (!outcome.report) {
    return row;
  }
  const Bench::Report& report = *outcome.report;
  row.wall_ns = report.wall_ns;
  row.cpu_ns = report.process_user_ns + report.process_kernel_ns;
  const bool disk_counts = report.disk.reads != 0;
  row.reads = disk_counts ? report.disk.reads : report.requested_reads;
  row.bytes = disk_counts ? report.disk.bytes : report.requested_bytes;
  return row;
}

std::string FormatTable(std::span<const EngineRow> rows) {
  constexpr size_t line_size = 160;
  std::string table;
  std::array<char, line_size> line{};
  auto add = [&](int length) {
    if (length > 0) {
      table.append(line.data(),
                   (std::min)(static_cast<size_t>(length), line.size() - 1));
    }
  };
  add(snprintf(line.data(), line.size(), "%-10s %5s %12s %12s %12s %10s %14s\n",
               "Engine", "Exit", "Wall (ms)", "CPU (ms)", "Entries", "Reads",
               "Bytes"));
  for (const EngineRow& row : rows) {
    add(snprintf(line.data(), line.size(),
                 "%-10s %5d %12.1f %12.1f %12llu %10llu %14llu\n",
                 std::string(EngineName(row.engine)).c_str(), row.exit_code,
                 static_cast<double>(row.wall_ns) / ns_per_ms,
                 static_cast<double>(row.cpu_ns) / ns_per_ms,
                 static_cast<unsigned long long>(row.entries),
                 static_cast<unsigned long long>(row.reads),
                 static_cast<unsigned long long>(row.bytes)));
  }
  return table;
}

std::string FormatScanTables(std::span<const EngineRow> rows) {
  const bool any = std::ranges::any_of(
      rows, [](const EngineRow& row) { return row.scan.has_value(); });
  if (!any) {
    return {};
  }
  std::string tables;
  FormatVerdictTable(rows, tables);
  FormatWorkerTable(rows, tables);
  FormatParallelismTable(rows, tables);
  return tables;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
