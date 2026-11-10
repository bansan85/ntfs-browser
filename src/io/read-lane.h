#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include <windows.h>

#include <ntfs-browser/namespace.h>

#include "io/async-read-pool.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// Alignment of a read ahead, in offset and length. A device takes only reads
// of whole sectors: this is a multiple of both sizes in use, 512 and 4096.
inline constexpr ULONGLONG read_alignment = 4096;

// The bytes of the aligned read that covers length bytes at offset. offset and
// length MUST leave read_alignment below the largest ULONGLONG.
[[nodiscard]] ULONGLONG AlignedReadBytes(ULONGLONG offset,
                                         ULONGLONG length) noexcept;

// One announced range, and the slot of its read once started.
struct AnnouncedRange {
  ULONGLONG offset;
  ULONGLONG length;
  std::optional<std::size_t> slot;
  // A read was served from it.
  bool used{false};
};

// A queue of announced ranges, and the reads started for them in a pool. The
// reads start in order, so the ranges whose read started are the first of them.
// The standing lane also follows which of its reads are in flight. Not thread
// safe: every call comes from one thread.
class ReadLane {
 public:
  // pool MUST outlive the lane. standing tells the lane of the standing queue
  // from the first queue.
  ReadLane(AsyncReadPool& pool, bool standing)
      : pool_(&pool), standing_(standing) {}

  // The ranges announced, whether their read started or not.
  [[nodiscard]] std::size_t Size() const { return ranges_.size(); }

  // The ranges whose read started, and the bytes of those reads.
  [[nodiscard]] std::size_t Started() const { return started_; }

  [[nodiscard]] ULONGLONG StartedBytes() const { return started_bytes_; }

  // Whether a range waits for its read to start.
  [[nodiscard]] bool HasWaiting() const { return started_ < ranges_.size(); }

  // The bytes of the aligned read of the first range that waits. HasWaiting()
  // MUST be true.
  [[nodiscard]] ULONGLONG WaitingBytes() const;

  // The range at index, which MUST be below Size().
  [[nodiscard]] AnnouncedRange& At(std::size_t index) {
    return ranges_.at(index);
  }

  [[nodiscard]] const AnnouncedRange& At(std::size_t index) const {
    return ranges_.at(index);
  }

  // Adds a range at the end of the queue. Its read does not start yet.
  void Add(ULONGLONG offset, ULONGLONG length);

  // Whether a range of exactly this offset and length is announced.
  [[nodiscard]] bool HoldsExactly(ULONGLONG offset, ULONGLONG length) const;

  // The index of the first of the first count ranges that holds [offset, end),
  // or count when none does.
  [[nodiscard]] std::size_t FindHolding(std::size_t count, ULONGLONG offset,
                                        ULONGLONG end) const;

  // Starts the read of the first range that waits, in a free slot of the pool.
  // A read that cannot start leaves the slot with nothing read.
  void StartNext();

  // Reads that are started and not finished, for the standing lane. A finished
  // read waits for its caller in its slot and costs only its bytes. It visits
  // the reads in flight, not every range the lane has started.
  [[nodiscard]] std::size_t InFlight();

  // Forgets the range at index, and cancels its read.
  void Drop(std::size_t index);

  // Forgets every range, and cancels the reads.
  void DropAll();

  // Forgets the ranges that overlap the interval. A length that runs past the
  // largest offset is cut there.
  void DropOverlapping(ULONGLONG offset, ULONGLONG length);

 private:
#ifndef NDEBUG
  // The same count as InFlight(), by visiting every range the lane has
  // started. It checks InFlight() in a debug build.
  [[nodiscard]] std::size_t InFlightByScan() const;
#endif

  AsyncReadPool* pool_;
  bool standing_;
  std::deque<AnnouncedRange> ranges_;
  // How many ranges at the front have a read started, and their bytes.
  std::size_t started_{0};
  ULONGLONG started_bytes_{0};
  // The slots of the standing lane's reads that were started and not seen
  // finished, at most the lane's share of the reads. A read leaves it when it
  // is seen finished or waited for, and when its range is dropped, before its
  // slot can be taken by another read.
  std::vector<std::size_t> reading_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
