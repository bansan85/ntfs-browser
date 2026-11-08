#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <ntfs-browser/namespace.h>

#include "arguments.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

// One line of the comparison table: what one engine did on the target.
struct EngineRow {
  Engine engine = Engine::MftList;
  int exit_code = 0;
  // Wall time of the session, and CPU time of the process (user and kernel).
  std::uint64_t wall_ns = 0;
  std::uint64_t cpu_ns = 0;
  std::uint64_t entries = 0;
  // Reads of the disk and their bytes. The reader's own count when it gives
  // one, else the reads the volume was asked for.
  std::uint64_t reads = 0;
  std::uint64_t bytes = 0;
  // What the run through the scan measured, with --via-scan.
  std::optional<ScanResult> scan;
};

// Makes the row of engine from what its run gave.
EngineRow MakeRow(Engine engine, const Outcome& outcome);

// The table of rows, a header line then one line per row, each ended by a
// newline. Times are in milliseconds with one decimal.
std::string FormatTable(std::span<const EngineRow> rows);

// The tables of the rows that ran through the scan, each a header line then
// one line per row: the verdicts (T_real, T_io, T_cpu, R and the critical
// path), the worker thread (where its time went) and the parallelism (CPU of
// all the threads). Empty when no row ran through the scan.
std::string FormatScanTables(std::span<const EngineRow> rows);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
