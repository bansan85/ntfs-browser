#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "internal-export.h"
#include "log/ntfs-common.h"
#include "log/worker-log.h"
#include "search/engine.h"
#include "search/entry-buffer.h"
#include "search/scan-probe.h"

namespace NTFS_BROWSER_NAMESPACE {

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
// The caller can sleep until the worker has news. Add(), SetPercent() and
// EndWork() count as news and wake WaitForNews(), and so does Wake(). The count
// and the flags sit under the same mutex as the buffers.
//
// The calls are split by thread. The worker calls BeginWork(), Add(),
// SetPercent() and EndWork(). The caller calls Prepare(), Take(), ReplayLog(),
// Release(), Percent() and WaitForNews(), one after the other and never from
// two threads. Wake() is the one call that any thread may make, at any time.
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

  using TimePoint = std::chrono::steady_clock::time_point;

  // Sleeps the caller until it has a reason to Take() again, and returns then.
  // The reasons are, in the order of their strength:
  //   - the worker ended, Wake() was called, or the buffer holds max_pending
  //     entries (the batch cannot grow): wakes at once;
  //   - the deadline: wakes then. No value means no deadline;
  //   - news since the last Take() (an entry, a percent): wakes, but not before
  //     news_not_before. A time in the past lets the news wake the call at
  //     once.
  // A Wake() that came while nobody waited is kept for the next call.
  void WaitForNews(TimePoint news_not_before,
                   std::optional<TimePoint> deadline);

  // Wakes WaitForNews() at once, or the next one if none waits. Callable from
  // any thread. This is how Cancel() reaches a caller that sleeps.
  void Wake() noexcept;

  // Forgets the entries of the last Take(). Their views die.
  void Release() noexcept;

  // The last percent the engine reported.
  [[nodiscard]] std::optional<double> Percent() const noexcept;

 private:
  // Add() under a probe (see scan-probe.h): the same steps, timed.
  void AddTimed(const Entry& entry, ScanTimings& timings);

  // Copies entry into the buffer the worker fills and counts the news. Call it
  // with mutex_ held and room in the buffer.
  void Store(const Entry& entry);

  // Counts one more piece of news and wakes the caller if it sleeps. Call it
  // with mutex_ held.
  void NoteNews();

  // A percent below zero means the engine reported none.
  static constexpr double no_percent = -1.0;

  std::size_t max_pending_;
  mutable std::mutex mutex_;
  std::condition_variable_any filled_;
  // Signals WaitForNews(). Always used with mutex_.
  std::condition_variable changed_;
  std::stop_token stop_;
  std::array<EntryBuffer, 2> buffers_;
  // The buffer the worker fills. The caller reads the other one between a
  // Take() and a Release(). Guarded by mutex_.
  std::size_t fill_{0};
  // The buffer that Release() clears, or null. Used by the caller only.
  EntryBuffer* reading_{nullptr};
  bool done_{false};
  // Counts the Add() and SetPercent() calls. Guarded by mutex_.
  std::uint64_t news_{0};
  // The value of news_ at the last Take(): news_ above it is not taken yet.
  // Guarded by mutex_.
  std::uint64_t taken_news_{0};
  // WaitForNews() sleeps: the worker only notifies then. Guarded by mutex_.
  bool waiting_{false};
  // The buffer the worker fills holds max_pending entries. Guarded by mutex_.
  bool full_{false};
  // Wake() was called and no WaitForNews() has consumed it. Guarded by mutex_.
  bool wake_{false};
  State result_{State::NotStarted};
  // Guarded by mutex_. A std::atomic<double> would need libatomic on a 32-bit
  // target.
  double percent_{no_percent};
  Log::Detail::WorkerLog log_;
};

}  // namespace NTFS_BROWSER_NAMESPACE
