#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Log::Detail {

// The log of one worker thread that runs while its owner goes on. The sinks
// are single-threaded, so the worker captures its lines and the owner thread
// replays them, in order.
//
// The capture is double buffered. The worker appends to one CapturedLog with no
// lock. Replay() asks for a hand-over, and the worker makes it at its next
// Checkpoint(): it swaps to the other log under a short lock, and the owner
// replays the full one. The owner never reads the log that the worker fills,
// and Replay() never waits for the worker. A line reaches the sinks one
// Replay() after the Checkpoint() that follows it, and every line has reached
// them once End() was called and Replay() ran.
//
// The calls are split by thread: Prepare() and Replay() belong to the owner,
// Begin(), Checkpoint() and End() to the worker.
class NTFS_BROWSER_EXPORT_TESTS_ONLY WorkerLog {
 public:
  WorkerLog() = default;
  WorkerLog(const WorkerLog&) = delete;
  WorkerLog& operator=(const WorkerLog&) = delete;
  WorkerLog(WorkerLog&&) = delete;
  WorkerLog& operator=(WorkerLog&&) = delete;
  ~WorkerLog() = default;

  // Forgets every line and keeps the levels in levels from now on. The owner
  // calls it before the worker starts. Take the levels from EnabledLevels():
  // the worker MUST NOT ask the sinks.
  void Prepare(LevelMask levels) noexcept;

  // The calling thread starts to capture. The worker calls it first.
  void Begin() noexcept;

  // Hands the lines over when the owner asked for them. Cheap when it did not.
  // The worker calls it often, at the points where it also hands over work.
  void Checkpoint() noexcept;

  // The calling thread stops to capture, and what is left is ready to replay.
  // The worker calls it last. Nothing is appended after it.
  void End() noexcept;

  // Replays the lines handed over so far to the sinks, on the calling thread,
  // in order. Asks the worker for the next lines. Takes the lock for a moment
  // only.
  void Replay() noexcept;

 private:
  // One capture, with the marks of the lines it holds.
  struct Slot {
    CapturedLog log;
    CapturedLog::Mark from;
    CapturedLog::Mark to;
  };

  // The slot to replay, or null when nothing is ready. Marks it as read.
  Slot* TakeReady() noexcept;

  // Forgets what slot held and frees it for the worker.
  void Release(Slot& slot) noexcept;

  // Seals the slot the worker fills and makes the worker fill the other one.
  // The lock MUST be held.
  void HandOver() noexcept;

  std::mutex mutex_;
  std::array<Slot, 2> slots_;
  // The slot the worker fills. The other one is free, ready or being read.
  std::size_t fill_{0};
  bool ready_{false};
  bool reading_{false};
  // The worker called End().
  bool finished_{false};
  LevelMask levels_{0};
  // The salvaged conditions the worker had counted at the last hand-over.
  std::uint64_t recovered_base_{0};
  // Set by Replay() when it found nothing, read by Checkpoint() without the
  // lock.
  std::atomic<bool> requested_{false};
  // Only the worker touches it.
  std::optional<CaptureScope> scope_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Log::Detail
