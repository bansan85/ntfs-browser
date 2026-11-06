#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// A clock that only moves when a model says so: nanoseconds from 0. It makes a
// simulated disk deterministic and instant, with no sleep.
class SimulatedClock {
 public:
  [[nodiscard]] std::uint64_t NowNs() const noexcept { return now_ns_; }

  // Moves the clock forward by ns: the caller worked for that long.
  void Advance(std::uint64_t ns) noexcept { now_ns_ += ns; }

  // Moves the clock to ns if it is later: the caller waited until then.
  void AdvanceTo(std::uint64_t ns) noexcept {
    if (ns > now_ns_) {
      now_ns_ = ns;
    }
  }

 private:
  std::uint64_t now_ns_{0};
};

// What a simulated disk costs. The defaults are those of a SATA SSD.
struct DiskModel {
  // Time to reach a position that does not follow the previous read issued.
  std::uint64_t seek_ns{80000};
  // Fixed cost of every read, adjacent or not.
  std::uint64_t per_read_ns{20000};
  // Bytes a single read streams at.
  std::uint64_t single_bytes_per_second{230'000'000};
  // Bytes all the reads together stream at, at most.
  std::uint64_t max_bytes_per_second{520'000'000};
  // Reads the disk serves at once.
  std::uint32_t lanes{8};
  // CPU time the consumer spends on each MiB a read hands it, to model parsing.
  std::uint64_t consume_ns_per_mib{0};
};

// What a ThrottledDiskReader has done.
struct ThrottleStats {
  // Reads issued to the disk, those announced and those made on the spot.
  std::uint64_t disk_reads{0};
  std::uint64_t disk_bytes{0};
  // ReadInto() calls served from an announced range, and those that were not.
  std::uint64_t served_reads{0};
  std::uint64_t unannounced_reads{0};
  // Time the caller waited for the disk, on the simulated clock.
  std::uint64_t wait_ns{0};
  // Announced ranges dropped before any read used them.
  std::uint64_t wasted_ranges{0};

  [[nodiscard]] bool operator==(const ThrottleStats&) const = default;
};

// Wraps a reader, and makes each read cost what a DiskModel says on a
// SimulatedClock: ReadInto() waits until the read completes, Prefetch() and
// PrefetchStanding() start reads ahead under the same caps, windows, matching
// rules and share of reads as Io::Win32DiskReader (Tuning::ReaderTuning), so
// that a change of a cap moves the simulated time the way it moves a real one.
// The data come from the wrapped reader. The same reads in the same order give
// the same time. Not thread safe.
class ThrottledDiskReader final : public IDiskReader {
 public:
  // The reader and the clock MUST outlive this object. caps are the read-ahead
  // caps by pattern, M1 in force until SetReadPattern() says otherwise.
  ThrottledDiskReader(
      std::unique_ptr<IDiskReader> inner, const DiskModel& model,
      SimulatedClock& clock,
      const std::array<Tuning::ReaderTuning, Tuning::pattern_count>& caps = {});

  bool Open(std::wstring_view path) override;
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;
  // Passes the question on: the simulated disk is of the kind the wrapped
  // reader says.
  [[nodiscard]] Tuning::MediaKind GetMediaKind() const override;
  void Prefetch(ULONGLONG offset, ULONGLONG length) const override;
  void CancelPrefetch() const override;
  void PrefetchStanding(ULONGLONG offset, ULONGLONG length) const override;
  void CancelStanding(ULONGLONG offset, ULONGLONG length) const override;
  void SetReadPattern(Tuning::Pattern pattern) const override;

  [[nodiscard]] const ThrottleStats& Stats() const noexcept { return stats_; }

 private:
  // One announced range, and the completion time of its read once started.
  struct Range {
    ULONGLONG offset;
    ULONGLONG length;
    std::optional<std::uint64_t> done_ns;
    bool used;
  };

  // A queue of announced ranges. The reads start in order, so the ranges whose
  // read started are the first of them.
  struct Queue {
    std::deque<Range> ranges;
    // How many ranges at the front have a read started, and their bytes.
    std::size_t started{0};
    ULONGLONG started_bytes{0};
  };

  // Schedules a read of bytes at offset on the disk, issued now. Returns when
  // it completes.
  [[nodiscard]] std::uint64_t Schedule(ULONGLONG offset, ULONGLONG bytes) const;

  // Starts the reads of the announced ranges, within the caps: the first queue
  // first, then the standing lane.
  void StartReads() const;

  // The first queue's half of StartReads(). The first read always starts: the
  // caller is about to wait for it.
  void StartFirstQueue() const;

  // The standing lane's half of StartReads().
  void StartStandingLane() const;

  // Whether a range of length bytes at offset may be announced.
  [[nodiscard]] bool Admissible(ULONGLONG offset, ULONGLONG length) const;

  // The most reads the standing lane may have in flight: the first queue keeps
  // a share of the reads cap for itself.
  [[nodiscard]] std::size_t StandingCap() const;

  // Reads of the standing lane that are started and not finished at the time
  // of the simulated clock. A finished read waits for its caller and costs
  // only its bytes.
  [[nodiscard]] std::size_t StandingInFlight() const;

  // Forgets the range of queue at index.
  void Drop(Queue& queue, std::size_t index) const;

  // The index of the first of the first count ranges of queue that holds
  // [offset, end), or count when none does.
  [[nodiscard]] static std::size_t FindHolding(const Queue& queue,
                                               std::size_t count,
                                               ULONGLONG offset, ULONGLONG end);

  // Waits for the read of the range of queue at index, which holds a read that
  // ends at end, and uses the range up. The ranges of the first queue before
  // it are dropped when passed_over is set. False when the read of the range
  // never started: the caller reads on its own then.
  [[nodiscard]] bool ServeFrom(Queue& queue, std::size_t index,
                               bool passed_over, ULONGLONG end) const;

  // Bytes of the aligned read a range makes.
  [[nodiscard]] static ULONGLONG AlignedBytes(const Range& range);

  std::unique_ptr<IDiskReader> inner_;
  DiskModel model_;
  SimulatedClock* clock_;
  std::array<Tuning::ReaderTuning, Tuning::pattern_count> caps_;

  mutable const Tuning::ReaderTuning* active_;
  mutable Queue first_;
  mutable Queue standing_;
  // When each lane is free, the time the device bandwidth is booked until, and
  // where the last read issued ended.
  mutable std::vector<std::uint64_t> lane_free_;
  mutable std::uint64_t bandwidth_free_{0};
  mutable std::optional<ULONGLONG> last_end_;
  mutable ThrottleStats stats_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
