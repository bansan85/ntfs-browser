#include "throttled-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Nanoseconds in a second.
constexpr std::uint64_t ns_per_second = 1'000'000'000;

// A read ahead is aligned to this, in offset and length, as a device needs.
constexpr ULONGLONG alignment = 4096;

// The share of the reads in flight, as a divisor of the reads cap, that the
// first queue keeps whatever the standing lane holds, and at least one: the
// value Io::Win32DiskReader uses.
constexpr std::size_t first_queue_reserve_divisor = 4;

// Bytes in a MiB, to price the consumer's work.
constexpr std::uint64_t bytes_per_mib = std::uint64_t{1} << 20U;

ULONGLONG AlignDown(ULONGLONG offset) { return offset - offset % alignment; }

ULONGLONG AlignUp(ULONGLONG end) { return AlignDown(end + alignment - 1); }

// Nanoseconds to move bytes at rate bytes per second, rounded up.
std::uint64_t Transfer(ULONGLONG bytes, std::uint64_t rate) {
  if (rate == 0) {
    return 0;
  }
  return (bytes * ns_per_second + rate - 1) / rate;
}

}  // namespace

ThrottledDiskReader::ThrottledDiskReader(
    std::unique_ptr<IDiskReader> inner, const DiskModel& model,
    SimulatedClock& clock,
    const std::array<Tuning::ReaderTuning, Tuning::pattern_count>& caps)
    : inner_(std::move(inner)),
      model_(model),
      clock_(&clock),
      caps_(caps),
      active_(caps_.data()),
      lane_free_((std::max<std::uint32_t>)(1, model.lanes), 0) {}

bool ThrottledDiskReader::Open(std::wstring_view path) {
  return inner_->Open(path);
}

std::optional<ULONGLONG> ThrottledDiskReader::Size() const {
  return inner_->Size();
}

Tuning::MediaKind ThrottledDiskReader::GetMediaKind() const {
  return inner_->GetMediaKind();
}

ULONGLONG ThrottledDiskReader::AlignedBytes(const Range& range) {
  return AlignUp(range.offset + range.length) - AlignDown(range.offset);
}

std::uint64_t ThrottledDiskReader::Schedule(ULONGLONG offset,
                                            ULONGLONG bytes) const {
  const auto lane = std::ranges::min_element(lane_free_);
  const std::uint64_t issue = clock_->NowNs();
  const std::uint64_t start = (std::max)(issue, *lane);
  const bool adjacent = last_end_ && *last_end_ == offset;
  const std::uint64_t service_end =
      start + model_.per_read_ns + (adjacent ? 0 : model_.seek_ns) +
      Transfer(bytes, model_.single_bytes_per_second);
  const std::uint64_t bandwidth_start = (std::max)(bandwidth_free_, start);
  const std::uint64_t bandwidth_end =
      bandwidth_start + Transfer(bytes, model_.max_bytes_per_second);
  const std::uint64_t done = (std::max)(service_end, bandwidth_end);
  bandwidth_free_ = bandwidth_end;
  *lane = done;
  last_end_ = offset + bytes;
  stats_.disk_reads++;
  stats_.disk_bytes += bytes;
  return done;
}

void ThrottledDiskReader::StartReads() const {
  const std::size_t lane_in_flight = StandingInFlight();
  while (first_.started < first_.ranges.size() &&
         first_.started + lane_in_flight < active_->max_reads) {
    Range& next = first_.ranges.at(first_.started);
    const ULONGLONG bytes = AlignedBytes(next);
    // The first one always starts: the caller is about to wait for it.
    if (first_.started != 0 &&
        first_.started_bytes + standing_.started_bytes + bytes >
            active_->max_bytes) {
      break;
    }
    next.done_ns = Schedule(AlignDown(next.offset), bytes);
    first_.started++;
    first_.started_bytes += bytes;
  }

  std::size_t in_flight = StandingInFlight();
  while (standing_.started < standing_.ranges.size() &&
         in_flight < StandingCap() &&
         first_.started + in_flight < active_->max_reads) {
    Range& next = standing_.ranges.at(standing_.started);
    const ULONGLONG bytes = AlignedBytes(next);
    // The lane stops at the first range that does not fit, so that the reads
    // start in the order of the announcements. Alone, a range always fits.
    if (first_.started_bytes + standing_.started_bytes != 0 &&
        first_.started_bytes + standing_.started_bytes + bytes >
            active_->max_bytes) {
      return;
    }
    next.done_ns = Schedule(AlignDown(next.offset), bytes);
    standing_.started++;
    standing_.started_bytes += bytes;
    in_flight++;
  }
}

std::size_t ThrottledDiskReader::StandingInFlight() const {
  const std::uint64_t now = clock_->NowNs();
  return gsl::narrow<std::size_t>(std::ranges::count_if(
      standing_.ranges | std::views::take(standing_.started),
      [now](const Range& range) {
        return range.done_ns && *range.done_ns > now;
      }));
}

std::size_t ThrottledDiskReader::StandingCap() const {
  const std::size_t cap = active_->max_reads;
  const std::size_t reserve =
      (std::max)(std::size_t{1}, cap / first_queue_reserve_divisor);
  return cap > reserve ? cap - reserve : 0;
}

void ThrottledDiskReader::Drop(Queue& queue, std::size_t index) const {
  const Range& range = queue.ranges.at(index);
  if (range.done_ns) {
    queue.started--;
    queue.started_bytes -= AlignedBytes(range);
    if (!range.used) {
      stats_.wasted_ranges++;
    }
  }
  queue.ranges.erase(queue.ranges.begin() + static_cast<std::ptrdiff_t>(index));
}

bool ThrottledDiskReader::Admissible(ULONGLONG offset, ULONGLONG length) const {
  constexpr ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
  if (offset > max_value - alignment ||
      length > max_value - alignment - offset) {
    return false;
  }
  const Range range{
      .offset = offset, .length = length, .done_ns = {}, .used = false};
  return AlignedBytes(range) <= active_->max_range_bytes;
}

void ThrottledDiskReader::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  if (length == 0 || first_.ranges.size() >= active_->max_announced_ranges ||
      !Admissible(offset, length)) {
    return;
  }
  // The lane reads it already: a second read would only cost the disk.
  if (std::ranges::any_of(standing_.ranges, [&](const Range& held) {
        return held.offset == offset && held.length == length;
      })) {
    return;
  }
  first_.ranges.push_back(
      {.offset = offset, .length = length, .done_ns = {}, .used = false});
  StartReads();
}

void ThrottledDiskReader::PrefetchStanding(ULONGLONG offset,
                                           ULONGLONG length) const {
  if (length == 0 || standing_.ranges.size() >= active_->max_announced_ranges ||
      !Admissible(offset, length)) {
    return;
  }
  standing_.ranges.push_back(
      {.offset = offset, .length = length, .done_ns = {}, .used = false});
  StartReads();
}

void ThrottledDiskReader::CancelPrefetch() const {
  while (!first_.ranges.empty()) {
    Drop(first_, 0);
  }
  // The reads that were freed go to the lane.
  StartReads();
}

void ThrottledDiskReader::CancelStanding(ULONGLONG offset,
                                         ULONGLONG length) const {
  constexpr ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
  if (length == 0) {
    return;
  }
  // An interval that runs past the largest offset is cut there.
  const ULONGLONG end =
      length > max_value - offset ? max_value : offset + length;
  std::size_t index = 0;
  while (index < standing_.ranges.size()) {
    const Range& range = standing_.ranges.at(index);
    const ULONGLONG range_end = range.length > max_value - range.offset
                                    ? max_value
                                    : range.offset + range.length;
    if (range.offset < end && offset < range_end) {
      Drop(standing_, index);
    } else {
      index++;
    }
  }
  StartReads();
}

void ThrottledDiskReader::SetReadPattern(Tuning::Pattern pattern) const {
  active_ = &caps_.at(static_cast<std::size_t>(pattern));
  StartReads();
  inner_->SetReadPattern(pattern);
}

std::size_t ThrottledDiskReader::FindHolding(const Queue& queue,
                                             std::size_t count,
                                             ULONGLONG offset, ULONGLONG end) {
  std::size_t match = 0;
  while (match < count && (offset < queue.ranges.at(match).offset ||
                           end > queue.ranges.at(match).offset +
                                     queue.ranges.at(match).length)) {
    match++;
  }
  return match;
}

bool ThrottledDiskReader::ServeFrom(Queue& queue, std::size_t index,
                                    bool passed_over, ULONGLONG end) const {
  if (passed_over) {
    for (std::size_t dropped = 0; dropped < index; dropped++) {
      Drop(queue, 0);
    }
    index = 0;
  }
  StartReads();
  Range& range = queue.ranges.at(index);
  // A range of the first queue that could not start before is started by
  // StartReads() above, because the first range always starts. One of the lane
  // may stay behind the caps: nothing was read ahead for the caller then.
  if (&queue == &standing_ && !range.done_ns) {
    Drop(queue, index);
    StartReads();
    return false;
  }

  const std::uint64_t before = clock_->NowNs();
  clock_->AdvanceTo(range.done_ns.value_or(before));
  stats_.wait_ns += clock_->NowNs() - before;
  stats_.served_reads++;
  range.used = true;
  if (end == range.offset + range.length) {
    Drop(queue, index);
  }
  StartReads();
  return true;
}

bool ThrottledDiskReader::ReadInto(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const {
  if (addr.QuadPart < 0) {
    return false;
  }
  const auto offset = static_cast<ULONGLONG>(addr.QuadPart);
  const ULONGLONG end = offset + dest.size();

  if (!dest.empty()) {
    const std::size_t searched =
        (std::min)(first_.ranges.size(), active_->match_lookahead);
    const std::size_t match = FindHolding(first_, searched, offset, end);
    const std::size_t held =
        FindHolding(standing_, standing_.ranges.size(), offset, end);
    bool served = false;
    if (match < searched) {
      served = ServeFrom(first_, match, true, end);
    } else if (held < standing_.ranges.size()) {
      served = ServeFrom(standing_, held, false, end);
    }
    if (!served) {
      const std::uint64_t before = clock_->NowNs();
      clock_->AdvanceTo(Schedule(offset, dest.size()));
      stats_.wait_ns += clock_->NowNs() - before;
      stats_.unannounced_reads++;
    }
  }

  clock_->Advance(model_.consume_ns_per_mib * dest.size() / bytes_per_mib);
  return inner_->ReadInto(addr, dest);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
