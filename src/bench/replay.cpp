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

// Plays the events of a log on a reader, in order.
class Replayer {
 public:
  Replayer(const IDiskReader& reader, const ReadLog& log,
           const ReplayOptions& options)
      : reader_(&reader),
        log_(&log),
        options_(&options),
        announce_(options.mode == ReplayMode::AllAnnounced) {
    std::uint64_t longest = 0;
    for (std::size_t index = 0; index < log.events.size(); index++) {
      const LogEvent& event = log.events.at(index);
      if (event.type == EventType::Read) {
        reads_.push_back(index);
        if (event.length <= max_replayed_read) {
          longest = (std::max)(longest, event.length);
        }
      }
    }
    buffer_.resize(static_cast<std::size_t>(longest));
  }

  ReplayResult Run() {
    const auto began = std::chrono::steady_clock::now();
    for (const LogEvent& event : log_->events) {
      if (event.type == EventType::Read) {
        if (announce_) {
          AnnounceAhead();
        }
        ReplayRead(*reader_, event, buffer_, result_);
        read_number_++;
      } else {
        Hint(event);
      }
    }
    result_.wall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - began)
            .count());
    return result_;
  }

 private:
  // Keeps the reads of the next turns announced, within the window.
  void AnnounceAhead() {
    const std::size_t until =
        (std::min)(reads_.size(), read_number_ + 1 + options_->announce_ahead);
    for (; announced_ < until; announced_++) {
      const LogEvent& next = log_->events.at(reads_.at(announced_));
      if (next.length != 0) {
        reader_->Prefetch(next.offset, next.length);
        result_.prefetches++;
      }
    }
  }

  // Replays an event that is no read. A mode that announces every read drops
  // the recorded announcements and withdrawals.
  void Hint(const LogEvent& event) {
    switch (event.type) {
      case EventType::Prefetch:
        if (!announce_) {
          reader_->Prefetch(event.offset, event.length);
          result_.prefetches++;
        }
        break;
      case EventType::Cancel:
        if (!announce_) {
          reader_->CancelPrefetch();
          result_.cancels++;
        }
        break;
      case EventType::StandingPrefetch:
        if (!announce_) {
          reader_->PrefetchStanding(event.offset, event.length);
          result_.standing_prefetches++;
        }
        break;
      case EventType::StandingCancel:
        if (!announce_) {
          reader_->CancelStanding(event.offset, event.length);
          result_.standing_cancels++;
        }
        break;
      case EventType::Pattern:
        if (event.length < Tuning::pattern_count) {
          reader_->SetReadPattern(static_cast<Tuning::Pattern>(event.length));
        }
        break;
      case EventType::Read:
      case EventType::PhaseBegin:
      case EventType::PhaseEnd:
        break;
    }
  }

  const IDiskReader* reader_;
  const ReadLog* log_;
  const ReplayOptions* options_;
  bool announce_;
  ReplayResult result_;
  // The index in the log of each read.
  std::vector<std::size_t> reads_;
  std::vector<BYTE> buffer_;
  std::size_t announced_{0};
  std::size_t read_number_{0};
};

}  // namespace

ReplayResult Replay(const IDiskReader& reader, const ReadLog& log,
                    const ReplayOptions& options) {
  return Replayer(reader, log, options).Run();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
