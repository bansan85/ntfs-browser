#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "bench/recorder.h"

namespace NTFS_BROWSER_NAMESPACE::Bench {

namespace {

// A read longer than this is not replayed: one buffer of its size would not
// fit, and a real run never makes one.
constexpr std::uint64_t max_replayed_read = std::uint64_t{1} << 30U;

// One read of the log, as the replay makes it.
void ReplayRead(const IDiskReader& reader, const LogEvent& event,
                std::vector<BYTE>& buffer, ReplayResult& result) {
  result.reads++;
  if (event.length > max_replayed_read ||
      event.offset >
          static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
    result.failed_reads++;
    return;
  }
  const std::span<BYTE> dest(buffer.data(),
                             static_cast<std::size_t>(event.length));
  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(event.offset);

  const bool session = Detail::SessionOn();
  const Detail::KindScope kind(event.kind);
  Detail::ResetReadNote();
  const std::uint64_t began = session ? Detail::NowNs() : 0;
  const bool ok = reader.ReadInto(addr, dest);
  if (session) {
    Detail::RecordRead(event.offset, event.length, began, Detail::NowNs());
  }
  if (!ok) {
    result.failed_reads++;
    return;
  }
  result.read_bytes += event.length;
}

}  // namespace

ReplayResult Replay(const IDiskReader& reader, const ReadLog& log,
                    const ReplayOptions& options) {
  ReplayResult result;

  std::vector<std::size_t> reads;
  std::uint64_t longest = 0;
  for (std::size_t index = 0; index < log.events.size(); index++) {
    const LogEvent& event = log.events.at(index);
    if (event.type == EventType::Read) {
      reads.push_back(index);
      if (event.length <= max_replayed_read) {
        longest = (std::max)(longest, event.length);
      }
    }
  }
  std::vector<BYTE> buffer(static_cast<std::size_t>(longest));

  const bool announce = options.mode == ReplayMode::AllAnnounced;
  std::size_t announced = 0;
  std::size_t read_number = 0;
  const auto began = std::chrono::steady_clock::now();
  for (const LogEvent& event : log.events) {
    switch (event.type) {
      case EventType::Read:
        if (announce) {
          // Keep the reads of the next turns announced, within the window.
          const std::size_t until =
              (std::min)(reads.size(),
                         read_number + 1 + options.announce_ahead);
          for (; announced < until; announced++) {
            const LogEvent& next = log.events.at(reads.at(announced));
            if (next.length != 0) {
              reader.Prefetch(next.offset, next.length);
              result.prefetches++;
            }
          }
        }
        ReplayRead(reader, event, buffer, result);
        read_number++;
        break;
      case EventType::Prefetch:
        if (!announce) {
          reader.Prefetch(event.offset, event.length);
          result.prefetches++;
        }
        break;
      case EventType::Cancel:
        if (!announce) {
          reader.CancelPrefetch();
          result.cancels++;
        }
        break;
      case EventType::StandingPrefetch:
        if (!announce) {
          reader.PrefetchStanding(event.offset, event.length);
          result.standing_prefetches++;
        }
        break;
      case EventType::StandingCancel:
        if (!announce) {
          reader.CancelStanding(event.offset, event.length);
          result.standing_cancels++;
        }
        break;
      case EventType::Pattern:
        if (event.length < Tuning::pattern_count) {
          reader.SetReadPattern(static_cast<Tuning::Pattern>(event.length));
        }
        break;
      case EventType::PhaseBegin:
      case EventType::PhaseEnd:
        break;
    }
  }
  result.wall_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - began)
          .count());
  return result;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
