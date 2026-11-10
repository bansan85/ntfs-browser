#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <windows.h>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "io/async-read-pool.h"
#include "io/read-lane.h"
#include "tuning/tuning.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// The read-ahead caps of each read pattern.
using ReaderCaps = std::array<Tuning::ReaderTuning, Tuning::pattern_count>;

// Reads the announced ranges ahead, in announcement order, through an
// overlapped handle of its own. Up to ReaderTuning::max_reads of them are in
// flight at once, so the disk reads them while the caller works. Two queues
// share those reads: the first, which a walk announces and withdraws all the
// time, and the standing lane, which only an explicit call empties. Every call
// comes from the caller's thread, so nothing is locked.
class Win32Prefetcher {
 public:
  // path is what the pool opens, on the first announcement.
  Win32Prefetcher(std::wstring path, const ReaderCaps& caps)
      : caps_(caps), pool_(std::move(path), MaxReadsOf(caps)) {}

  Win32Prefetcher(const Win32Prefetcher&) = delete;
  Win32Prefetcher& operator=(const Win32Prefetcher&) = delete;
  Win32Prefetcher(Win32Prefetcher&&) = delete;
  Win32Prefetcher& operator=(Win32Prefetcher&&) = delete;

  // Drops every range, so that no read is in flight when the pool goes.
  ~Win32Prefetcher();

  // Adds a range to the first queue. A range that the standing lane holds as it
  // is, offset and length, is ignored: the lane reads it already.
  void Announce(ULONGLONG offset, ULONGLONG length);

  // Adds a range to the standing lane.
  void AnnounceStanding(ULONGLONG offset, ULONGLONG length);

  // Copies the bytes at offset into dest when an announced range holds them
  // all and its read succeeded. The first queue is tried before the standing
  // lane. False otherwise: the caller then reads them itself, and reports a
  // failure itself.
  [[nodiscard]] bool Serve(ULONGLONG offset, std::span<BYTE> dest);

  // Forgets the first queue. The standing lane stays.
  void Cancel();

  // Forgets the ranges of the standing lane that overlap the interval.
  void CancelStanding(ULONGLONG offset, ULONGLONG length);

  // Reads of the slots in flight and not finished. Only a session needs it.
  [[nodiscard]] std::uint32_t InFlightReads() const { return pool_.InFlight(); }

  // Switches the caps to those of pattern. The reads in flight finish whatever
  // the new caps say, and the ranges waiting start under them.
  void SetPattern(Tuning::Pattern pattern);

 private:
  // The most reads in flight any pattern allows: the slots the first queue
  // needs.
  static std::size_t MaxReadsOf(const ReaderCaps& caps);

  // The range that holds a read: its lane and place. The ranges of the first
  // queue before it were announced for reads that did not come, and are passed
  // over.
  struct Match {
    ReadLane* lane;
    std::size_t index;
    bool passed_over;
  };

  // What a read finds on arrival at its range, to tell the caller's wait from
  // the CPU's. Left as it is when no session measures.
  struct Arrival {
    // A session measures this read.
    bool bench{false};
    // The read of the range was in flight, or not started.
    bool had_to_wait{false};
    // Announced ranges were held back by the caps.
    bool window_held_back{false};
    // The reads in flight, this one included.
    std::uint32_t in_flight{0};

    [[nodiscard]] Bench::ReadClass Class() const {
      if (had_to_wait) {
        return Bench::ReadClass::Io;
      }
      return window_held_back ? Bench::ReadClass::Window
                              : Bench::ReadClass::Ready;
    }
  };

  // Whether a range of length bytes at offset may be announced: its aligned
  // read neither wraps nor outgrows the cap on one range.
  [[nodiscard]] bool Admissible(ULONGLONG offset, ULONGLONG length) const;

  // Starts the reads of the next ranges, within the caps: the first queue
  // first, then the standing lane.
  void StartReads();

  // Whether a read of bytes, added to the bytes of every read started, stays
  // within the byte cap of the active caps.
  [[nodiscard]] bool FitsBytesCap(ULONGLONG bytes) const {
    return first_.StartedBytes() + standing_.StartedBytes() + bytes <=
           active_->max_bytes;
  }

  // Starts the reads of the standing lane, in its order, under the caps and the
  // share of reads in flight it may take.
  void StartStandingReads();

  // The range that holds the read of dest.size() bytes at offset, if any. The
  // first queue is searched before the standing lane.
  [[nodiscard]] std::optional<Match> FindMatch(ULONGLONG offset,
                                               ULONGLONG length);

  // Serves a read from the range that match names, which holds it.
  [[nodiscard]] bool ServeFrom(const Match& match, ULONGLONG offset,
                               std::span<BYTE> dest);

  // What the read of the range of lane at index finds. Judged before anything
  // is dropped or started.
  [[nodiscard]] Arrival JudgeArrival(const ReadLane& lane,
                                     std::size_t index) const;

  // Waits for the read of range and copies the bytes at offset out of its
  // slot. False when the range has no read, or the read did not return them
  // all. A served read is noted when a session measures.
  [[nodiscard]] bool TakeFromSlot(AnnouncedRange& range, const Arrival& arrival,
                                  ULONGLONG offset, std::span<BYTE> dest);

  // The most reads that may be in flight at once under the active caps.
  [[nodiscard]] std::size_t ReadsCap() const { return active_->max_reads; }

  // The most reads the standing lane may have in flight: the first queue always
  // finds its share of the reads free, whatever the lane holds.
  [[nodiscard]] std::size_t StandingCap() const;

  ReaderCaps caps_;
  // The caps of the read pattern in force: an element of caps_.
  const Tuning::ReaderTuning* active_{caps_.data()};
  AsyncReadPool pool_;

  ReadLane first_{pool_, false};
  ReadLane standing_{pool_, true};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
