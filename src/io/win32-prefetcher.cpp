#include "io/win32-prefetcher.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "io/async-read-pool.h"
#include "io/read-lane.h"
#include "tuning/tuning.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// The share of the reads in flight, as a divisor of the reads cap, that the
// first queue keeps whatever the standing lane holds, and at least one. A
// quarter keeps the reads that a caller waits for from ever finding the disk
// queue full of far reads. The gain of the lane hardly moves with a share from
// none to half.
constexpr std::size_t first_queue_reserve_divisor = 4;

}  // namespace

std::size_t Win32Prefetcher::MaxReadsOf(const ReaderCaps& caps) {
  std::size_t most = 1;
  for (const Tuning::ReaderTuning& tuning : caps) {
    most = (std::max)(most, tuning.max_reads);
  }
  return most;
}

std::size_t Win32Prefetcher::StandingCap() const {
  const std::size_t cap = ReadsCap();
  const std::size_t reserve =
      (std::max)(std::size_t{1}, cap / first_queue_reserve_divisor);
  return cap > reserve ? cap - reserve : 0;
}

Win32Prefetcher::~Win32Prefetcher() {
  // The lane first: a read of the first queue that is dropped must not start
  // one of the lane's in its place.
  standing_.DropAll();
  Cancel();
}

bool Win32Prefetcher::Admissible(ULONGLONG offset, ULONGLONG length) const {
  return offset <= (std::numeric_limits<ULONGLONG>::max)() - read_alignment &&
         length <= (std::numeric_limits<ULONGLONG>::max)() - read_alignment -
                       offset &&
         AlignedReadBytes(offset, length) <= active_->max_range_bytes;
}

void Win32Prefetcher::Announce(ULONGLONG offset, ULONGLONG length) {
  // The aligned read MUST NOT wrap, and MUST fit the cap on its own. The lane
  // may read it already: a second read would only cost the disk.
  if (length == 0 || first_.Size() >= active_->max_announced_ranges ||
      !Admissible(offset, length) || standing_.HoldsExactly(offset, length) ||
      !pool_.EnsureOpen()) {
    return;
  }

  first_.Add(offset, length);
  StartReads();
}

void Win32Prefetcher::AnnounceStanding(ULONGLONG offset, ULONGLONG length) {
  if (length == 0 || standing_.Size() >= active_->max_announced_ranges ||
      !Admissible(offset, length) || !pool_.EnsureOpen()) {
    return;
  }

  standing_.Add(offset, length);
  StartReads();
}

void Win32Prefetcher::StartReads() {
  const std::size_t lane_in_flight = standing_.InFlight();
  while (first_.HasWaiting() &&
         first_.Started() + lane_in_flight < ReadsCap()) {
    // The first one always starts: the caller is about to wait for it.
    if (first_.Started() != 0 && !FitsBytesCap(first_.WaitingBytes())) {
      break;
    }
    first_.StartNext();
  }
  StartStandingReads();
}

void Win32Prefetcher::StartStandingReads() {
  std::size_t in_flight = standing_.InFlight();
  while (standing_.HasWaiting() && in_flight < StandingCap() &&
         first_.Started() + in_flight < ReadsCap()) {
    // The lane stops at the first range that does not fit, so that the reads
    // start in the order of the announcements. Alone, a range always fits.
    if (first_.StartedBytes() + standing_.StartedBytes() != 0 &&
        !FitsBytesCap(standing_.WaitingBytes())) {
      return;
    }
    standing_.StartNext();
    in_flight++;
  }
}

void Win32Prefetcher::SetPattern(Tuning::Pattern pattern) {
  active_ = &caps_.at(static_cast<std::size_t>(pattern));
  StartReads();
}

std::optional<Win32Prefetcher::Match>
    Win32Prefetcher::FindMatch(ULONGLONG offset, ULONGLONG length) {
  const ULONGLONG end = offset + length;
  const std::size_t searched =
      (std::min)(first_.Size(), active_->match_lookahead);
  const std::size_t match = first_.FindHolding(searched, offset, end);
  if (match < searched) {
    return Match{.lane = &first_, .index = match, .passed_over = true};
  }
  // A range of the lane is never passed over: it is used up, or withdrawn.
  const std::size_t held = standing_.FindHolding(standing_.Size(), offset, end);
  if (held < standing_.Size()) {
    return Match{.lane = &standing_, .index = held, .passed_over = false};
  }
  return std::nullopt;
}

bool Win32Prefetcher::Serve(ULONGLONG offset, std::span<BYTE> dest) {
  if (dest.empty()) {
    return false;
  }
  const std::optional<Match> match = FindMatch(offset, dest.size());
  return match && ServeFrom(*match, offset, dest);
}

Win32Prefetcher::Arrival
    Win32Prefetcher::JudgeArrival(const ReadLane& lane,
                                  std::size_t index) const {
  const AnnouncedRange& matched = lane.At(index);
  return {.bench = true,
          .had_to_wait = !matched.slot || pool_.Reading(*matched.slot),
          .window_held_back = lane.HasWaiting(),
          .in_flight = pool_.InFlight() + 1};
}

bool Win32Prefetcher::TakeFromSlot(AnnouncedRange& range,
                                   const Arrival& arrival, ULONGLONG offset,
                                   std::span<BYTE> dest) {
  if (!range.slot) {
    return false;
  }
  const std::uint64_t waited_from = arrival.bench ? Bench::Detail::NowNs() : 0;
  const DWORD got = pool_.Wait(*range.slot);
  const std::uint64_t wait_ns = arrival.bench && arrival.had_to_wait
                                    ? Bench::Detail::NowNs() - waited_from
                                    : 0;
  const bool served = pool_.CopyOut(*range.slot, got, offset, dest);
  range.used = range.used || served;
  if (served && arrival.bench) {
    Bench::Detail::NoteRead(arrival.Class(), wait_ns, arrival.in_flight);
  }
  return served;
}

bool Win32Prefetcher::ServeFrom(const Match& match, ULONGLONG offset,
                                std::span<BYTE> dest) {
  ReadLane& lane = *match.lane;
  std::size_t index = match.index;
  const ULONGLONG end = offset + dest.size();

  const Arrival arrival =
      Bench::Detail::SessionOn() ? JudgeArrival(lane, index) : Arrival{};

  // The ranges of the first queue announced before it were passed over.
  if (match.passed_over) {
    for (std::size_t dropped = 0; dropped < index; dropped++) {
      lane.Drop(0);
    }
    index = 0;
  }
  StartReads();

  AnnouncedRange& range = lane.At(index);
  const bool served = TakeFromSlot(range, arrival, offset, dest);

  // A read up to the end of its range uses it up. A failed one is not tried
  // again: the caller's own reads report the failure.
  if (!served || end == range.offset + range.length) {
    lane.Drop(index);
  }
  StartReads();
  return served;
}

void Win32Prefetcher::Cancel() {
  first_.DropAll();
  // The slots that were freed go to the lane.
  StartReads();
}

void Win32Prefetcher::CancelStanding(ULONGLONG offset, ULONGLONG length) {
  standing_.DropOverlapping(offset, length);
  StartReads();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
