#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsscanbench/via-scan.h>
#include <ntfs-browser/namespace.h>

#include "bench/bench.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench;
namespace LibBench = NTFS_BROWSER_NAMESPACE::Bench;
using Catch::Approx;

TEST_CASE("A run whose CPU hides under the I/O is bound by the I/O",
          "[search][verdict]") {
  Bench::ScanFigures figures;
  figures.wall_ns = 100;
  figures.io_ns = 100;
  figures.cpu_ns = 40;

  const Bench::Verdict verdict = Bench::Judge(figures);

  CHECK(verdict.path == Bench::CriticalPath::IoStrong);
  CHECK(verdict.io_share == Approx(1.0));
  REQUIRE(verdict.overlap.has_value());
  CHECK(*verdict.overlap == Approx(1.0));
}

TEST_CASE("A run that sits on its CPU floor is bound by the CPU",
          "[search][verdict]") {
  Bench::ScanFigures figures;
  figures.wall_ns = 105;
  figures.io_ns = 40;
  figures.cpu_ns = 100;

  const Bench::Verdict verdict = Bench::Judge(figures);

  CHECK(verdict.path == Bench::CriticalPath::Cpu);
  CHECK(verdict.io_share < 0.5);
}

TEST_CASE("A run that does the I/O and the CPU one after the other is mixed",
          "[search][verdict]") {
  Bench::ScanFigures figures;
  figures.wall_ns = 140;
  figures.io_ns = 100;
  figures.cpu_ns = 40;

  const Bench::Verdict verdict = Bench::Judge(figures);

  CHECK(verdict.path == Bench::CriticalPath::Mixed);
  REQUIRE(verdict.overlap.has_value());
  CHECK(*verdict.overlap == Approx(0.0));
}

TEST_CASE("A run within ten percent of its I/O floor is bound by the I/O",
          "[search][verdict]") {
  Bench::ScanFigures figures;
  figures.wall_ns = 108;
  figures.io_ns = 100;
  figures.cpu_ns = 40;

  CHECK(Bench::Judge(figures).path == Bench::CriticalPath::Io);
}

TEST_CASE("A run without T_io or T_cpu has no critical path or overlap",
          "[search][verdict]") {
  Bench::ScanFigures figures;
  figures.wall_ns = 100;
  figures.io_ns = 50;

  const Bench::Verdict verdict = Bench::Judge(figures);

  CHECK(verdict.path == Bench::CriticalPath::Unknown);
  CHECK_FALSE(verdict.overlap.has_value());
  CHECK(verdict.io_share == Approx(0.5));
}

TEST_CASE("The parallel figures divide the CPU by the time and the threads",
          "[search][verdict]") {
  Bench::ScanFigures figures;
  figures.wall_ns = 100;
  figures.total_cpu_ns = 200;
  figures.worker_cpu_ns = 80;
  figures.threads = 4;

  const Bench::Verdict verdict = Bench::Judge(figures);

  CHECK(verdict.cores_busy == Approx(2.0));
  CHECK(verdict.parallel_efficiency == Approx(0.5));
  CHECK(verdict.worker_cpu_share == Approx(0.8));
  CHECK(verdict.worker_cpu_of_total == Approx(0.4));
}

TEST_CASE("Every ratio is 0 when its divisor is", "[search][verdict]") {
  const Bench::Verdict verdict = Bench::Judge({});

  CHECK(verdict.io_share == 0.0);
  CHECK(verdict.cores_busy == 0.0);
  CHECK(verdict.parallel_efficiency == 0.0);
  CHECK(verdict.worker_cpu_share == 0.0);
  CHECK(verdict.worker_cpu_of_total == 0.0);
}

TEST_CASE("The figures of a report take the main thread as the caller",
          "[search][verdict]") {
  LibBench::Report report;
  report.wall_ns = 1000;
  report.process_user_ns = 300;
  report.process_kernel_ns = 100;
  report.threads.push_back({.role = LibBench::ThreadRole::Other,
                            .user_ns = 250,
                            .kernel_ns = 50,
                            .wall_ns = 900});
  report.threads.push_back({.role = LibBench::ThreadRole::Main,
                            .user_ns = 20,
                            .kernel_ns = 10,
                            .wall_ns = 1000});
  Bench::ScanMeasure measure;
  measure.worker.cpu_ns = 300;

  const Bench::ScanFigures figures = Bench::FiguresOf(report, measure);

  CHECK(figures.wall_ns == 1000);
  CHECK(figures.total_cpu_ns == 400);
  CHECK(figures.caller_cpu_ns == 30);
  CHECK(figures.worker_cpu_ns == 300);
  CHECK(figures.threads == 2);
}

TEST_CASE("The critical paths have names", "[search][verdict]") {
  CHECK(Bench::CriticalPathName(Bench::CriticalPath::Unknown) == "unknown");
  CHECK(Bench::CriticalPathName(Bench::CriticalPath::IoStrong) == "io-strong");
  CHECK(Bench::CriticalPathName(Bench::CriticalPath::Io) == "io");
  CHECK(Bench::CriticalPathName(Bench::CriticalPath::Cpu) == "cpu");
  CHECK(Bench::CriticalPathName(Bench::CriticalPath::Mixed) == "mixed");
}
