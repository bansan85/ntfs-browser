#include "alloc/cluster-bitmap.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <exception>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// Bits in one bitmap byte.
constexpr ULONGLONG bits_per_byte = 8;

// Windows that may fail before the rest of the bitmap is given up on. A hostile
// volume can claim a bitmap of petabytes: reading it window by window, and
// remembering each failure, would take forever and all the memory. Past this
// many failures, a window that was not read is unknown without being tried.
constexpr size_t max_failed_windows = 4096;

// Window size a test asked for. 0 means the default.
size_t window_bytes_override = 0;

// Most bytes any window has held since the override was last set.
size_t peak_bytes_held = 0;

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

}  // namespace

ClusterBitmap::ClusterBitmap(Reader reader, ULONGLONG data_size,
                             ULONGLONG cluster_count, size_t window_bytes)
    : reader_(std::move(reader)),
      data_size_(data_size),
      data_bits_(BitsOf(data_size)),
      limit_(cluster_count != 0 ? cluster_count : data_bits_),
      window_bytes_(window_bytes != 0 ? window_bytes : default_window_bytes),
      shorter_than_volume_(cluster_count != 0 && data_bits_ < cluster_count) {}

ClusterState ClusterBitmap::State(ULONGLONG lcn) { return RangeState(lcn, 1); }

ClusterState ClusterBitmap::RangeState(ULONGLONG lcn, ULONGLONG clusters) {
  RangeQuery query{.lcn = lcn, .clusters = clusters};
  StatesInOrder({&query, 1});
  return query.state;
}

// Reads the window at index unless it is the one held. A failed or short read
// keeps what came back, and the window is remembered so it is not read again.
const ClusterBitmap::Window* ClusterBitmap::Load(ULONGLONG index) {
  if (have_window_ && window_.index == index) {
    return &window_;
  }
  if (failed_.contains(index) || failed_.size() >= max_failed_windows) {
    return nullptr;
  }

  const ULONGLONG offset = index * window_bytes_;
  const auto length = static_cast<size_t>(
      std::min<ULONGLONG>(window_bytes_, data_size_ - offset));

  window_.index = index;
  window_.bytes.assign(length, 0);
  window_.valid = 0;
  have_window_ = true;
  peak_bytes_held = std::max(peak_bytes_held, window_.bytes.size());

  std::optional<ULONGLONG> read;
  try {
    read = reader_(offset, window_.bytes);
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
  window_.valid = read ? static_cast<size_t>(std::min<ULONGLONG>(*read, length))
                       : size_t{0};
  if (window_.valid < length) {
    failed_.insert(index);
    Log::Debug("Cluster bitmap window {} read {} of {} bytes", index,
               window_.valid, length);
  }
  return &window_;
}

ClusterBitmap::Counts ClusterBitmap::Count(ULONGLONG first, ULONGLONG last) {
  Counts counts;
  const ULONGLONG window_bits = ULONGLONG{window_bytes_} * bits_per_byte;

  ULONGLONG bit = first;
  while (bit < last) {
    // Past the end of the bitmap nothing can be judged, whatever the claimed
    // size of the volume: do not walk it window by window.
    if (bit >= data_bits_) {
      counts.unknown += last - bit;
      break;
    }
    const ULONGLONG index = bit / window_bits;
    const ULONGLONG base = index * window_bits;
    // The next window starts at base + window_bits; clamp when that wraps.
    const ULONGLONG stop =
        window_bits > std::numeric_limits<ULONGLONG>::max() - base
            ? last
            : std::min(last, base + window_bits);

    // Bits past the end of the bitmap cannot be judged.
    const ULONGLONG known_stop = std::min(stop, data_bits_);
    ULONGLONG judged_stop = bit;
    if (bit < known_stop) {
      const Window* window = Load(index);
      if (window != nullptr) {
        judged_stop = std::min(known_stop,
                               base + ULONGLONG{window->valid} * bits_per_byte);
        if (judged_stop > bit) {
          const ULONGLONG set =
              CountSet(window->bytes, bit - base, judged_stop - base);
          counts.allocated += set;
          counts.free += (judged_stop - bit) - set;
        }
      }
    }
    counts.unknown += stop - std::max(bit, judged_stop);
    bit = stop;

    // Too many windows failed: the rest of the range is not worth walking.
    if (failed_.size() >= max_failed_windows && bit < last) {
      counts.unknown += last - bit;
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

    if (counts.allocated > 0) {
      query.state = ClusterState::Allocated;
    } else if (beyond) {
      query.state = ClusterState::OutOfRange;
    } else if (counts.unknown > 0) {
      query.state = ClusterState::Unknown;
    } else {
      query.state = ClusterState::Free;
    }
  }
}

std::optional<SpaceSummary> ClusterBitmap::Summary(DWORD cluster_size) {
  if (limit_ == 0) {
    return std::nullopt;
  }
  const Counts counts = Count(0, limit_);
  if (counts.allocated + counts.free == 0) {
    return std::nullopt;
  }

  SpaceSummary summary;
  summary.total_clusters = limit_;
  summary.free_clusters = counts.free;
  summary.unknown_clusters = counts.unknown;
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
          .failed_windows = failed_.size()};
}

size_t ClusterBitmap::BytesHeld() const noexcept {
  return have_window_ ? window_.bytes.size() : size_t{0};
}

void SetBitmapWindowBytesForTests(size_t bytes) {
  window_bytes_override = bytes;
  peak_bytes_held = 0;
}

size_t BitmapWindowBytes() {
  return window_bytes_override != 0 ? window_bytes_override
                                    : default_window_bytes;
}

size_t PeakBitmapBytesHeld() { return peak_bytes_held; }

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
