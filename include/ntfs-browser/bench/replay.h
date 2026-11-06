#pragma once

#include <cstddef>
#include <cstdint>

#include <ntfs-browser/bench/read-log.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Bench {

// How Replay() issues the announcements.
enum class ReplayMode : std::uint8_t {
  // The Prefetch(), CancelPrefetch(), PrefetchStanding(), CancelStanding()
  // and SetReadPattern() calls of the log, in their place among the reads: the
  // run as it was, with no parsing.
  AsRecorded,
  // The announcements of the log, the standing lane's included, are dropped,
  // and every read is announced ahead of its turn instead, as far as
  // announce_ahead allows: the floor of a plan that knows every read in
  // advance.
  AllAnnounced
};

struct ReplayOptions {
  ReplayMode mode{ReplayMode::AsRecorded};
  // AllAnnounced: reads kept announced ahead of the one being made. It MUST
  // stay below the reader's cap on announced ranges, or the reader drops hints.
  std::size_t announce_ahead{512};
};

struct ReplayResult {
  // From the first event to the last, on the steady clock.
  std::uint64_t wall_ns{0};
  std::uint64_t reads{0};
  std::uint64_t read_bytes{0};
  // Reads that failed, or that were too long to replay.
  std::uint64_t failed_reads{0};
  std::uint64_t prefetches{0};
  std::uint64_t cancels{0};
  // The standing lane calls made: ranges announced, and withdrawals.
  std::uint64_t standing_prefetches{0};
  std::uint64_t standing_cancels{0};
};

// Replays the reads of log through reader, in order, with nothing between
// them: no parsing, no output. The time it takes is what the disk costs of the
// run the log was taken from. When a session runs, the replayed reads are
// timed and classified like the real ones. reader MUST be open.
[[nodiscard]] NTFS_BROWSER_EXPORT ReplayResult
    Replay(const IDiskReader& reader, const ReadLog& log,
           const ReplayOptions& options = {});

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
