#include "alloc/cluster-bitmap.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/bad-cluster-list.h"
#include "alloc/types.h"
#include "log/ntfs-common.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// Bits in one bitmap byte.
constexpr ULONGLONG bits_per_byte = 8;

// Window size a test asked for. 0 means the default.
size_t window_bytes_override = 0;

// Bits of data_size bytes, saturated when the product overflows.
ULONGLONG BitsOf(ULONGLONG data_size) noexcept {
  return data_size > std::numeric_limits<ULONGLONG>::max() / bits_per_byte
             ? std::numeric_limits<ULONGLONG>::max()
             : data_size * bits_per_byte;
}

// Set bits among bits [first, last) of bytes. first and last count from the
// first bit of bytes[0], and last MUST NOT pass the end of bytes.
ULONGLONG CountSet(std::span<const BYTE> bytes, ULONGLONG first,
                   ULONGLONG last) noexcept {
  ULONGLONG set = 0;
  for (ULONGLONG bit = first; bit < last;) {
    const ULONGLONG byte_index = bit / bits_per_byte;
    const auto from = static_cast<unsigned>(bit % bits_per_byte);
    const ULONGLONG byte_end = (byte_index + 1) * bits_per_byte;
    const auto to = static_cast<unsigned>(std::min(last, byte_end) -
                                          byte_index * bits_per_byte);
    // Bits [from, to) of this byte, as a mask.
    const auto mask =
        static_cast<unsigned>(((1U << to) - 1U) & ~((1U << from) - 1U));
    set += static_cast<ULONGLONG>(std::popcount(
        static_cast<unsigned>(bytes[static_cast<size_t>(byte_index)]) & mask));
    bit = std::min(last, byte_end);
  }
  return set;
}

// First cluster past the extent, saturated when it would wrap past 2^64.
ULONGLONG EndOf(const Extent& extent) noexcept {
  return extent.lcn > std::numeric_limits<ULONGLONG>::max() - extent.clusters
             ? std::numeric_limits<ULONGLONG>::max()
             : extent.lcn + extent.clusters;
}

// Appends the run [lcn, lcn + clusters) to runs, joined to the last one when it
// has the same state and ends where this one starts.
void AppendRun(std::vector<ClusterRun>& runs, ULONGLONG lcn, ULONGLONG clusters,
               ClusterState state) {
  if (clusters == 0) {
    return;
  }
  if (!runs.empty() && runs.back().state == state &&
      runs.back().lcn + runs.back().clusters == lcn) {
    runs.back().clusters += clusters;
    return;
  }
  runs.push_back({.lcn = lcn, .clusters = clusters, .state = state});
}

// Bits [first, last) of bytes. They count from the first bit of bytes[0], and
// last MUST NOT pass the end of bytes.
struct BitSpan {
  std::span<const BYTE> bytes;
  ULONGLONG first;
  ULONGLONG last;
};

// Bit number bit of bytes: 0 or 1. bit MUST lie inside bytes.
unsigned BitAt(std::span<const BYTE> bytes, ULONGLONG bit) {
  const auto byte = static_cast<unsigned>(bytes[static_cast<size_t>(
      bit /
      bits_per_byte)]);  // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return (byte >> (bit % bits_per_byte)) & 1U;
}

// End of the run of bits equal to set that starts at bit, within span. Whole
// bytes of the same value are skipped at once.
ULONGLONG BitRunEnd(const BitSpan& span, ULONGLONG bit, bool set) {
  const unsigned whole = set ? 0xFFU : 0x00U;
  ULONGLONG end = bit + 1;
  while (end < span.last) {
    if (end % bits_per_byte == 0 && span.last - end >= bits_per_byte &&
        static_cast<unsigned>(span.bytes[static_cast<size_t>(
            end /
            bits_per_byte)]) ==  // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            whole) {
      end += bits_per_byte;
      continue;
    }
    if ((BitAt(span.bytes, end) != 0) != set) {
      break;
    }
    end++;
  }
  return end;
}

// Appends the runs of span as clusters from lcn_base + the bit number.
void AppendBitRuns(std::vector<ClusterRun>& runs, const BitSpan& span,
                   ULONGLONG lcn_base) {
  ULONGLONG bit = span.first;
  while (bit < span.last) {
    const bool set = BitAt(span.bytes, bit) != 0;
    const ULONGLONG end = BitRunEnd(span, bit, set);
    AppendRun(runs, lcn_base + bit, end - bit,
              set ? ClusterState::Allocated : ClusterState::Free);
    bit = end;
  }
}

// Merges one run with the bad extents that overlap it, appending the pieces to
// merged: Bad where an extent covers the run, the run's own state elsewhere.
// next_bad is the first extent that may still reach the run, and moves forward
// as runs come in LCN order. Returns whether a bad extent covered a Free run:
// the bad list and the bitmap then disagree.
bool OverlayRun(const ClusterRun& run, std::span<const Extent> overlapping,
                size_t& next_bad, std::vector<ClusterRun>& merged) {
  // Callers keep next_bad below overlapping.size().
  const auto bad_at = [&overlapping](size_t index) -> const Extent& {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return overlapping[index];
  };
  bool free_covered = false;
  ULONGLONG at = run.lcn;
  const ULONGLONG run_end = run.lcn + run.clusters;
  while (at < run_end) {
    while (next_bad < overlapping.size() && EndOf(bad_at(next_bad)) <= at) {
      next_bad++;
    }
    if (next_bad == overlapping.size() || bad_at(next_bad).lcn >= run_end) {
      AppendRun(merged, at, run_end - at, run.state);
      break;
    }
    const Extent& bad = bad_at(next_bad);
    if (bad.lcn > at) {
      AppendRun(merged, at, bad.lcn - at, run.state);
      at = bad.lcn;
    }
    const ULONGLONG to = std::min(EndOf(bad), run_end);
    free_covered = free_covered || run.state == ClusterState::Free;
    AppendRun(merged, at, to - at, ClusterState::Bad);
    at = to;
  }
  return free_covered;
}

}  // namespace

ClusterBitmap::ClusterBitmap(Reader reader, ULONGLONG data_size,
                             ULONGLONG cluster_count, size_t window_bytes)
    : windows_(std::move(reader), data_size,
               window_bytes != 0 ? window_bytes : default_window_bytes),
      data_bits_(BitsOf(data_size)),
      limit_(cluster_count != 0 ? cluster_count : data_bits_),
      shorter_than_volume_(cluster_count != 0 && data_bits_ < cluster_count) {}

void ClusterBitmap::SetBadList(const BadClusterList* list) noexcept {
  bad_ = list;
}

ClusterState ClusterBitmap::State(ULONGLONG lcn) { return RangeState(lcn, 1); }

ClusterState ClusterBitmap::RangeState(ULONGLONG lcn, ULONGLONG clusters) {
  RangeQuery query{.lcn = lcn, .clusters = clusters};
  StatesInOrder({&query, 1});
  return query.state;
}

void ClusterBitmap::ApplyBad(Counts& counts, ULONGLONG first, ULONGLONG last,
                             const JudgedBits& judged) {
  if (bad_ == nullptr) {
    return;
  }
  for (const Extent& extent : bad_->Overlapping(first, last)) {
    const ULONGLONG from = std::max(extent.lcn, first);
    const ULONGLONG to = std::min(EndOf(extent), last);
    const ULONGLONG judged_to = std::min(to, judged.stop);
    ULONGLONG unjudged_from = from;
    if (judged_to > from && judged.window != nullptr) {
      const ULONGLONG set = CountSet(judged.window->bytes, from - judged.base,
                                     judged_to - judged.base);
      const ULONGLONG clear = (judged_to - from) - set;
      counts.allocated -= set;
      counts.free -= clear;
      counts.bad += judged_to - from;
      bad_with_clear_bit_ = bad_with_clear_bit_ || clear != 0;
      unjudged_from = judged_to;
    }
    if (to > unjudged_from) {
      counts.unknown -= to - unjudged_from;
      counts.bad += to - unjudged_from;
    }
  }
}

ClusterBitmap::Slice ClusterBitmap::ReadSlice(ULONGLONG bit, ULONGLONG last) {
  const ULONGLONG window_bits =
      ULONGLONG{windows_.WindowBytes()} * bits_per_byte;
  const ULONGLONG index = bit / window_bits;
  const ULONGLONG base = index * window_bits;
  // The next window starts at base + window_bits; clamp when that wraps.
  const ULONGLONG stop =
      window_bits > std::numeric_limits<ULONGLONG>::max() - base
          ? last
          : std::min(last, base + window_bits);
  Slice slice{.judged = {.window = nullptr, .base = base, .stop = bit},
              .stop = stop};

  // Bits past the end of the bitmap cannot be judged.
  const ULONGLONG known_stop = std::min(stop, data_bits_);
  if (bit >= known_stop) {
    return slice;
  }
  const Window* window = windows_.Load(index);
  if (window == nullptr) {
    return slice;
  }
  const ULONGLONG judged_stop =
      std::min(known_stop, base + ULONGLONG{window->valid} * bits_per_byte);
  if (judged_stop > bit) {
    slice.judged = {.window = window, .base = base, .stop = judged_stop};
  }
  return slice;
}

void ClusterBitmap::CountUnknown(Counts& counts, ULONGLONG first,
                                 ULONGLONG last) {
  counts.unknown += last - first;
  ApplyBad(counts, first, last, JudgedBits{nullptr, 0, first});
}

ClusterBitmap::Counts ClusterBitmap::Count(ULONGLONG first, ULONGLONG last) {
  Counts counts;
  ULONGLONG bit = first;
  while (bit < last) {
    // Past the end of the bitmap nothing can be judged, whatever the claimed
    // size of the volume: do not walk it window by window.
    if (bit >= data_bits_) {
      CountUnknown(counts, bit, last);
      break;
    }
    const Slice slice = ReadSlice(bit, last);
    const JudgedBits& judged = slice.judged;
    if (judged.window != nullptr) {
      const ULONGLONG set = CountSet(judged.window->bytes, bit - judged.base,
                                     judged.stop - judged.base);
      counts.allocated += set;
      counts.free += (judged.stop - bit) - set;
      counts.judged += judged.stop - bit;
    }
    counts.unknown += slice.stop - judged.stop;
    ApplyBad(counts, bit, slice.stop, judged);
    bit = slice.stop;

    // Too many windows failed: the rest of the range is not worth walking.
    if (windows_.GaveUp() && bit < last) {
      CountUnknown(counts, bit, last);
      break;
    }
  }
  return counts;
}

void ClusterBitmap::StatesInOrder(std::span<RangeQuery> queries) {
  std::vector<size_t> order(queries.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::ranges::stable_sort(order, [&queries](size_t left, size_t right) {
    return queries[left].lcn < queries[right].lcn;
  });

  for (const size_t at : order) {
    RangeQuery& query = queries[at];
    query.allocated_clusters = 0;
    query.free_clusters = 0;
    query.bad_clusters = 0;

    if (query.clusters == 0) {
      query.state = ClusterState::Free;
      continue;
    }
    if (query.lcn >= limit_) {
      query.state = ClusterState::OutOfRange;
      continue;
    }

    // The part of the range that lies inside the volume is always judged.
    const bool beyond = query.clusters > limit_ - query.lcn;
    const ULONGLONG end = beyond ? limit_ : query.lcn + query.clusters;
    const Counts counts = Count(query.lcn, end);
    query.allocated_clusters = counts.allocated;
    query.free_clusters = counts.free;
    query.bad_clusters = counts.bad;

    if (counts.allocated > 0) {
      query.state = ClusterState::Allocated;
    } else if (counts.bad > 0) {
      query.state = ClusterState::Bad;
    } else if (beyond) {
      query.state = ClusterState::OutOfRange;
    } else if (counts.unknown > 0) {
      query.state = ClusterState::Unknown;
    } else {
      query.state = ClusterState::Free;
    }
  }
}

void ClusterBitmap::BitRuns(ULONGLONG first, ULONGLONG last,
                            std::vector<ClusterRun>& runs) {
  ULONGLONG bit = first;
  while (bit < last) {
    // Past the end of the bitmap nothing can be judged.
    if (bit >= data_bits_) {
      AppendRun(runs, bit, last - bit, ClusterState::Unknown);
      return;
    }
    const Slice slice = ReadSlice(bit, last);
    const JudgedBits& judged = slice.judged;
    if (judged.window != nullptr) {
      AppendBitRuns(runs,
                    {.bytes = judged.window->bytes,
                     .first = bit - judged.base,
                     .last = judged.stop - judged.base},
                    judged.base);
    }
    AppendRun(runs, judged.stop, slice.stop - judged.stop,
              ClusterState::Unknown);
    bit = slice.stop;

    // Too many windows failed: the rest of the range is not worth walking.
    if (windows_.GaveUp() && bit < last) {
      AppendRun(runs, bit, last - bit, ClusterState::Unknown);
      return;
    }
  }
}

void ClusterBitmap::OverlayBad(std::vector<ClusterRun>& runs) {
  if (bad_ == nullptr || runs.empty()) {
    return;
  }
  const ULONGLONG first = runs.front().lcn;
  const ULONGLONG last = runs.back().lcn + runs.back().clusters;
  const std::span<const Extent> overlapping = bad_->Overlapping(first, last);
  if (overlapping.empty()) {
    return;
  }

  std::vector<ClusterRun> merged;
  merged.reserve(runs.size() + overlapping.size() * 2);
  size_t next_bad = 0;
  for (const ClusterRun& run : runs) {
    bad_with_clear_bit_ =
        OverlayRun(run, overlapping, next_bad, merged) || bad_with_clear_bit_;
  }
  runs = std::move(merged);
}

void ClusterBitmap::StateRuns(ULONGLONG lcn, ULONGLONG clusters,
                              std::vector<ClusterRun>& runs) {
  runs.clear();
  if (clusters == 0) {
    return;
  }
  if (lcn >= limit_) {
    runs.push_back(
        {.lcn = lcn, .clusters = clusters, .state = ClusterState::OutOfRange});
    return;
  }
  const bool beyond = clusters > limit_ - lcn;
  const ULONGLONG end = beyond ? limit_ : lcn + clusters;
  BitRuns(lcn, end, runs);
  OverlayBad(runs);
  if (beyond) {
    AppendRun(runs, end, clusters - (end - lcn), ClusterState::OutOfRange);
  }
}

std::optional<SpaceSummary> ClusterBitmap::Summary(DWORD cluster_size) {
  if (limit_ == 0) {
    return std::nullopt;
  }
  const Counts counts = Count(0, limit_);
  // Nothing was read from the bitmap, so nothing can be said of free space:
  // listed bad clusters alone do not make a report.
  if (counts.judged == 0) {
    return std::nullopt;
  }

  SpaceSummary summary;
  summary.total_clusters = limit_;
  summary.free_clusters = counts.free;
  summary.unknown_clusters = counts.unknown;
  summary.bad_clusters = counts.bad;
  if (cluster_size != 0 &&
      counts.bad > std::numeric_limits<ULONGLONG>::max() / cluster_size) {
    Log::Warn("Bad space in bytes overflows 64 bits: reported saturated");
    summary.bad_bytes = std::numeric_limits<ULONGLONG>::max();
  } else {
    summary.bad_bytes = counts.bad * cluster_size;
  }
  if (cluster_size != 0 &&
      counts.free > std::numeric_limits<ULONGLONG>::max() / cluster_size) {
    Log::Warn("Free space in bytes overflows 64 bits: reported saturated");
    summary.free_bytes = std::numeric_limits<ULONGLONG>::max();
  } else {
    summary.free_bytes = counts.free * cluster_size;
  }
  return summary;
}

ClusterBitmap::Status ClusterBitmap::GetStatus() const noexcept {
  return {.shorter_than_volume = shorter_than_volume_,
          .failed_windows = windows_.FailedCount(),
          .bad_with_clear_bit = bad_with_clear_bit_};
}

void ClusterBitmap::SetRetention(size_t max_bytes) {
  windows_.SetRetention(max_bytes);
}

size_t ClusterBitmap::BytesHeld() const noexcept {
  return windows_.BytesHeld();
}

void SetBitmapWindowBytesForTests(size_t bytes) {
  window_bytes_override = bytes;
  ResetPeakBytesHeld();
}

size_t BitmapWindowBytes() {
  return window_bytes_override != 0 ? window_bytes_override
                                    : default_window_bytes;
}

size_t BitmapWindowBytes(size_t tuned) {
  return window_bytes_override != 0 ? window_bytes_override : tuned;
}

size_t PeakBitmapBytesHeld() { return PeakBytesHeld(); }

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
