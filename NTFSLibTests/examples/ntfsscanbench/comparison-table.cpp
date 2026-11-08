#include "comparison-table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>

#include "arguments.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// Nanoseconds in a millisecond, for the time columns.
constexpr double ns_per_ms = 1e6;

}  // namespace

EngineRow MakeRow(Engine engine, const Outcome& outcome) {
  EngineRow row;
  row.engine = engine;
  row.exit_code = outcome.exit_code;
  row.entries = outcome.entries;
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

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
