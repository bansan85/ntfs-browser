#include "io/read-lane.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "io/async-read-pool.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// The offset of the first byte of an aligned read that covers offset.
constexpr ULONGLONG AlignDown(ULONGLONG offset) noexcept {
  return offset - offset % read_alignment;
}

// The end of an aligned read that covers the byte before end. end MUST be at
// most read_alignment below the largest ULONGLONG.
constexpr ULONGLONG AlignUp(ULONGLONG end) noexcept {
  return AlignDown(end + read_alignment - 1);
}

// The bytes the aligned read of range covers.
ULONGLONG AlignedBytes(const AnnouncedRange& range) noexcept {
  return AlignUp(range.offset + range.length) - AlignDown(range.offset);
}

}  // namespace

ULONGLONG AlignedReadBytes(ULONGLONG offset, ULONGLONG length) noexcept {
  return AlignUp(offset + length) - AlignDown(offset);
}

ULONGLONG ReadLane::WaitingBytes() const {
  return AlignedBytes(ranges_.at(started_));
}

void ReadLane::Add(ULONGLONG offset, ULONGLONG length) {
  ranges_.push_back({.offset = offset, .length = length, .slot = {}});
}

bool ReadLane::HoldsExactly(ULONGLONG offset, ULONGLONG length) const {
  return std::ranges::any_of(ranges_, [&](const AnnouncedRange& held) {
    return held.offset == offset && held.length == length;
  });
}

std::size_t ReadLane::FindHolding(std::size_t count, ULONGLONG offset,
                                  ULONGLONG end) const {
  std::size_t match = 0;
  while (match < count &&
         (offset < ranges_.at(match).offset ||
          end > ranges_.at(match).offset + ranges_.at(match).length)) {
    match++;
  }
  return match;
}

void ReadLane::StartNext() {
  AnnouncedRange& next = ranges_.at(started_);
  const ULONGLONG bytes = AlignedBytes(next);
  next.slot = pool_->Acquire();
  pool_->Start(*next.slot, AlignDown(next.offset),
               gsl::narrow<std::size_t>(bytes),
               standing_ ? Bench::Detail::DiskReadOrigin::Standing
                         : Bench::Detail::DiskReadOrigin::ReadAhead);
  if (standing_ && pool_->Pending(*next.slot)) {
    reading_.push_back(*next.slot);
  }
  started_++;
  started_bytes_ += bytes;
}

std::size_t ReadLane::InFlight() {
#ifndef NDEBUG
  // A read finishes and never starts again, so the count only falls while
  // this runs: it lies between the two visits.
  const std::size_t before = InFlightByScan();
#endif
  // A finished read never reverts to in flight, so it leaves for good.
  std::erase_if(reading_,
                [this](std::size_t held) { return !pool_->Reading(held); });
#ifndef NDEBUG
  const std::size_t after = InFlightByScan();
  assert(after <= reading_.size() && reading_.size() <= before);
#endif
  return reading_.size();
}

#ifndef NDEBUG
std::size_t ReadLane::InFlightByScan() const {
  std::size_t reading = 0;
  for (std::size_t index = 0; index < started_; index++) {
    const std::optional<std::size_t>& held = ranges_.at(index).slot;
    if (held && pool_->Reading(*held)) {
      reading++;
    }
  }
  return reading;
}
#endif

void ReadLane::Drop(std::size_t index) {
  const AnnouncedRange& range = ranges_.at(index);
  if (range.slot) {
    if (standing_) {
      // Before the slot is free for another read.
      std::erase(reading_, *range.slot);
    }
    pool_->Release(*range.slot, standing_);
    const ULONGLONG bytes = AlignedBytes(range);
    started_--;
    started_bytes_ -= bytes;
    if (!range.used) {
      Bench::Detail::NoteDiskWaste(bytes, standing_);
    }
  }
  ranges_.erase(ranges_.begin() + static_cast<std::ptrdiff_t>(index));
}

void ReadLane::DropAll() {
  while (!ranges_.empty()) {
    Drop(0);
  }
}

void ReadLane::DropOverlapping(ULONGLONG offset, ULONGLONG length) {
  constexpr ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
  if (length == 0) {
    return;
  }
  // An interval that runs past the largest offset is cut there.
  const ULONGLONG end =
      length > max_value - offset ? max_value : offset + length;
  std::size_t index = 0;
  while (index < ranges_.size()) {
    const AnnouncedRange& range = ranges_.at(index);
    const ULONGLONG range_end = range.length > max_value - range.offset
                                    ? max_value
                                    : range.offset + range.length;
    if (range.offset < end && offset < range_end) {
      Drop(index);
    } else {
      index++;
    }
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
