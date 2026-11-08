#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "internal-export.h"
#include "log/ntfs-common.h"
#include "log/worker-log.h"
#include "search/engine.h"
#include "search/entry-buffer.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// Everything a scan passes from its worker thread to the thread that polls it:
// the entries, the percent and the log lines. It is the sink of the engine.
//
// The entries are double buffered. The worker fills one EntryBuffer. Take()
// swaps to the other, empty one under a short lock and gives the caller the
// full one, as views: no name and no path is copied. The caller reads it while
// the worker fills the other, and Release() forgets it for the next swap. The
// worker never writes to a buffer the caller reads. Add() holds the lock for
// the copy of one entry, so Take() waits for that at most.
//
// A worker that is faster than the caller does not make the buffer grow
// without end when max_pending is set: Add() waits for the next Take() once the
// buffer holds that many entries. A request on the stop token given to
// Prepare() ends the wait.
//
// The calls are split by thread. The worker calls BeginWork(), Add(),
// SetPercent() and EndWork(). The caller calls Prepare(), Take(), ReplayLog(),
// Release() and Percent(), one after the other and never from two threads.
class NTFS_BROWSER_EXPORT_TESTS_ONLY ScanExchange final : public EntrySink {
 public:
  // What Take() hands over.
  struct Taken {
    // The entries found since the previous Take(). Views into a buffer of the
    // exchange: valid until Release().
    std::span<const Entry> entries;
    // The worker has ended and this is its last batch: nothing follows.
    bool last{false};
    // How the engine ended. Only set with last.
    State result{State::NotStarted};
  };

  // max_pending is the most entries that wait for a Take(). 0 sets no limit.
  explicit ScanExchange(std::size_t max_pending);

  // Gets the exchange ready for a worker. The caller calls it before the worker
  // starts. levels are the log levels the worker keeps, see
  // Log::Detail::EnabledLevels(). stop ends the wait of a worker held back by
  // max_pending.
  void Prepare(Log::Detail::LevelMask levels, std::stop_token stop);

  // The worker thread starts to capture its log. It calls it first.
  void BeginWork() noexcept;

  // Stores a copy of entry. May wait, see max_pending. Throws what the copy
  // throws.
  void Add(const Entry& entry) override;

  void SetPercent(double percent) override;

  // The worker is done and result is how it ended. It calls it last. What the
  // worker found is ready for the caller from now on.
  void EndWork(State result) noexcept;

  // Gives the caller the entries found since the last call, possibly none, and
  // asks the worker for the next log lines. Release() MUST follow before the
  // next call. Never waits for the worker.
  [[nodiscard]] Taken Take();

  // Replays the log lines the worker handed over on the calling thread. Call it
  // after Take(): a batch that has last set finds the log of the worker done
  // and replays all of it.
  void ReplayLog() noexcept;

  // Forgets the entries of the last Take(). Their views die.
  void Release() noexcept;

  // The last percent the engine reported.
  [[nodiscard]] std::optional<double> Percent() const noexcept;

 private:
  // A percent below zero means the engine reported none.
  static constexpr double no_percent = -1.0;

  std::size_t max_pending_;
  std::mutex mutex_;
  std::condition_variable_any filled_;
  std::stop_token stop_;
  std::array<EntryBuffer, 2> buffers_;
  // The buffer the worker fills. The caller reads the other one between a
  // Take() and a Release(). Guarded by mutex_.
  std::size_t fill_{0};
  // The buffer that Release() clears, or null. Used by the caller only.
  EntryBuffer* reading_{nullptr};
  bool done_{false};
  State result_{State::NotStarted};
  std::atomic<double> percent_{no_percent};
  Log::Detail::WorkerLog log_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Search
