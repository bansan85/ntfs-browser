#pragma once

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

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
