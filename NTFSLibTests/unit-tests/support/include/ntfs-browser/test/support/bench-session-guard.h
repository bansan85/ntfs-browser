#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Runs a Bench session for the life of the object. The session is process
// wide, so a test that starts one MUST end it however the test ends.
class BenchSessionGuard final {
 public:
  explicit BenchSessionGuard(const Bench::SessionOptions& options = {}) {
    Bench::Start(options);
  }

  BenchSessionGuard(const BenchSessionGuard&) = delete;
  BenchSessionGuard& operator=(const BenchSessionGuard&) = delete;
  BenchSessionGuard(BenchSessionGuard&&) = delete;
  BenchSessionGuard& operator=(BenchSessionGuard&&) = delete;

  ~BenchSessionGuard() {
    if (running_) {
      try {
        static_cast<void>(Bench::Stop());
      } catch (...) {
        // A failing Stop() cannot be reported from a destructor.
      }
    }
  }

  // Ends the session and returns its report. The log stays for
  // Bench::TakeReadLog().
  [[nodiscard]] Bench::Report Finish() {
    running_ = false;
    return Bench::Stop();
  }

 private:
  bool running_{true};
};

// Asks the library to stop after a phase for the life of the object. The
// request is process wide and nothing else removes it, so a test that makes
// one MUST remove it however the test ends.
class StopAfterGuard final {
 public:
  explicit StopAfterGuard(Bench::Phase phase) { Bench::SetStopAfter(phase); }

  StopAfterGuard(const StopAfterGuard&) = delete;
  StopAfterGuard& operator=(const StopAfterGuard&) = delete;
  StopAfterGuard(StopAfterGuard&&) = delete;
  StopAfterGuard& operator=(StopAfterGuard&&) = delete;

  ~StopAfterGuard() { Bench::SetStopAfter(std::nullopt); }
};

// Makes sink the capture sink of the library for the life of the object.
class CaptureSinkGuard final {
 public:
  explicit CaptureSinkGuard(std::shared_ptr<Bench::ICaptureSink> sink) {
    Bench::SetCaptureSink(std::move(sink));
  }

  CaptureSinkGuard(const CaptureSinkGuard&) = delete;
  CaptureSinkGuard& operator=(const CaptureSinkGuard&) = delete;
  CaptureSinkGuard(CaptureSinkGuard&&) = delete;
  CaptureSinkGuard& operator=(CaptureSinkGuard&&) = delete;

  ~CaptureSinkGuard() { Bench::SetCaptureSink(nullptr); }
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
