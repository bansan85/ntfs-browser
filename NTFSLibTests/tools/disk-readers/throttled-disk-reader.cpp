#include "throttled-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Nanoseconds in a second.
constexpr std::uint64_t ns_per_second = 1'000'000'000;

// A read ahead is aligned to this, in offset and length, as a device needs.
constexpr ULONGLONG alignment = 4096;

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
  while (started_ < ranges_.size() && started_ < active_->max_reads) {
    Range& next = ranges_.at(started_);
    const ULONGLONG bytes = AlignedBytes(next);
    // The first one always starts: the caller is about to wait for it.
    if (started_ != 0 && started_bytes_ + bytes > active_->max_bytes) {
      return;
    }
    next.done_ns = Schedule(AlignDown(next.offset), bytes);
    started_++;
    started_bytes_ += bytes;
  }
}

void ThrottledDiskReader::DropFront() const {
  const Range& front = ranges_.front();
  if (front.done_ns) {
    started_--;
    started_bytes_ -= AlignedBytes(front);
    if (!front.used) {
      stats_.wasted_ranges++;
    }
  }
  ranges_.pop_front();
}

void ThrottledDiskReader::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  constexpr ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
  if (length == 0 || ranges_.size() >= active_->max_announced_ranges ||
      offset > max_value - alignment ||
      length > max_value - alignment - offset) {
    return;
  }
  const Range range{
      .offset = offset, .length = length, .done_ns = {}, .used = false};
  if (AlignedBytes(range) > active_->max_range_bytes) {
    return;
  }
  ranges_.push_back(range);
  StartReads();
}

void ThrottledDiskReader::CancelPrefetch() const {
  while (!ranges_.empty()) {
    DropFront();
  }
}

void ThrottledDiskReader::SetReadPattern(Tuning::Pattern pattern) const {
  active_ = &caps_.at(static_cast<std::size_t>(pattern));
  StartReads();
  inner_->SetReadPattern(pattern);
}

bool ThrottledDiskReader::ReadInto(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const {
  if (addr.QuadPart < 0) {
    return false;
  }
  const auto offset = static_cast<ULONGLONG>(addr.QuadPart);
  const ULONGLONG end = offset + dest.size();

  const std::size_t searched =
      (std::min)(ranges_.size(), active_->match_lookahead);
  std::size_t match = 0;
  while (match < searched &&
         (offset < ranges_.at(match).offset ||
          end > ranges_.at(match).offset + ranges_.at(match).length)) {
    match++;
  }

  if (!dest.empty() && match < searched) {
    for (std::size_t dropped = 0; dropped < match; dropped++) {
      DropFront();
    }
    StartReads();
    Range& range = ranges_.front();
    const std::uint64_t before = clock_->NowNs();
    // A range that could not start before is started by StartReads() above,
    // because the first range always starts.
    clock_->AdvanceTo(range.done_ns.value_or(before));
    stats_.wait_ns += clock_->NowNs() - before;
    stats_.served_reads++;
    range.used = true;
    if (end == range.offset + range.length) {
      DropFront();
    }
    StartReads();
  } else if (!dest.empty()) {
    const std::uint64_t before = clock_->NowNs();
    clock_->AdvanceTo(Schedule(offset, dest.size()));
    stats_.wait_ns += clock_->NowNs() - before;
    stats_.unannounced_reads++;
  }

  clock_->Advance(model_.consume_ns_per_mib * dest.size() / bytes_per_mib);
  return inner_->ReadInto(addr, dest);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
