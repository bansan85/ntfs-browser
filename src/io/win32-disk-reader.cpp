#include "io/win32-disk-reader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>
#include <winioctl.h>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "io/extended-path.h"
#include "io/host-media-kind.h"
#include "log/ntfs-common.h"
#include "tuning/tuning.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Alignment of a read ahead, in offset and length. A device takes only reads
// of whole sectors: this is a multiple of both sizes in use, 512 and 4096.
constexpr ULONGLONG read_alignment = 4096;

// The share of the reads in flight, as a divisor of the reads cap, that the
// first queue keeps whatever the standing lane holds, and at least one. A
// quarter keeps the reads that a caller waits for from ever finding the disk
// queue full of far reads. The gain of the lane hardly moves with a share from
// none to half.
constexpr size_t first_queue_reserve_divisor = 4;

// The largest buffer that a slot keeps once the standing lane is done with it.
// The lane holds up to a hundred slots at a time, so a slot MUST NOT keep the
// buffer of one big range for the small ranges that follow. 64 KiB is the
// widest run that the probes of a directory read in the usual case.
constexpr size_t lane_buffer_keep_bytes = 64 * 1024;

// The offset of the first byte of an aligned read that covers offset.
constexpr ULONGLONG AlignDown(ULONGLONG offset) noexcept {
  return offset - offset % read_alignment;
}

// The end of an aligned read that covers the byte before end. end MUST be at
// most read_alignment below the largest ULONGLONG.
constexpr ULONGLONG AlignUp(ULONGLONG end) noexcept {
  return AlignDown(end + read_alignment - 1);
}

// The probe a test installed in place of the system's answer, or null.
std::atomic<ReadDoneProbe> read_done_probe{nullptr};

// Whether the read behind overlapped is over. It never reverts: a read that is
// over stays over until its slot starts another.
bool ReadDone(const OVERLAPPED& overlapped) noexcept {
  const ReadDoneProbe probe = read_done_probe.load(std::memory_order_relaxed);
  return probe != nullptr ? probe(overlapped)
                          : HasOverlappedIoCompleted(&overlapped);
}

// True for the handle of a file, false for a volume or a disk: those answer
// for themselves, and a volume has no file information.
bool IsPlainFile(HANDLE handle) noexcept {
  BY_HANDLE_FILE_INFORMATION info{};
  return GetFileType(handle) == FILE_TYPE_DISK &&
         GetFileInformationByHandle(handle, &info) != FALSE &&
         (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

ReadDoneProbe SetReadDoneProbeForTests(ReadDoneProbe probe) noexcept {
  return read_done_probe.exchange(probe);
}

// Reads the announced ranges ahead, in announcement order, through an
// overlapped handle of its own. Up to ReaderTuning::max_reads of them are in
// flight at once, so the disk reads them while the caller works. Two queues
// share those reads: the first, which a walk announces and withdraws all the
// time, and the standing lane, which only an explicit call empties. Every call
// comes from the caller's thread, so nothing is locked.
class Win32DiskReader::Prefetcher {
 public:
  Prefetcher(std::wstring path, const ReaderCaps& caps)
      : path_(std::move(path)), caps_(caps), slots_(MaxReadsOf(caps)) {}

  Prefetcher(const Prefetcher&) = delete;
  Prefetcher& operator=(const Prefetcher&) = delete;
  Prefetcher(Prefetcher&&) = delete;
  Prefetcher& operator=(Prefetcher&&) = delete;
  ~Prefetcher();

  // Adds a range to the first queue. A range that the standing lane holds as it
  // is, offset and length, is ignored: the lane reads it already.
  void Announce(ULONGLONG offset, ULONGLONG length);

  // Adds a range to the standing lane.
  void AnnounceStanding(ULONGLONG offset, ULONGLONG length);

  // Copies the bytes at offset into dest when an announced range holds them
  // all and its read succeeded. The first queue is tried before the standing
  // lane. False otherwise: the caller then reads them itself, and reports a
  // failure itself.
  [[nodiscard]] bool Serve(ULONGLONG offset, std::span<BYTE> dest);

  // Forgets the first queue. The standing lane stays.
  void Cancel();

  // Forgets the ranges of the standing lane that overlap the interval.
  void CancelStanding(ULONGLONG offset, ULONGLONG length);

  // Reads of the slots in flight and not finished. Only a session needs it.
  [[nodiscard]] std::uint32_t InFlightReads() const { return InFlight(); }

  // Switches the caps to those of pattern. The reads in flight finish whatever
  // the new caps say, and the ranges waiting start under them.
  void SetPattern(Tuning::Pattern pattern);

 private:
  // The most reads in flight any pattern allows: the slots the first queue
  // needs.
  static size_t MaxReadsOf(const ReaderCaps& caps) {
    size_t most = 1;
    for (const Tuning::ReaderTuning& tuning : caps) {
      most = (std::max)(most, tuning.max_reads);
    }
    return most;
  }

  // One read ahead and its buffer, kept for the next read.
  struct Slot {
    OVERLAPPED overlapped{};
    HANDLE event{nullptr};
    BYTE* buffer{nullptr};
    size_t capacity{0};
    // Disk offset of the first byte of buffer.
    ULONGLONG start{0};
    // Held by an announced range.
    bool in_use{false};
    // A read was started and not waited for: it MUST be, before the buffer is
    // used or freed.
    bool pending{false};
    // Bytes the read returned, once waited for. 0 when it failed.
    DWORD got{0};
  };

  // One announced range, and the slot of its read once started.
  struct Range {
    ULONGLONG offset;
    ULONGLONG length;
    std::optional<size_t> slot;
    // A read was served from it.
    bool used{false};
  };

  // A queue of announced ranges. The reads start in order, so the ranges whose
  // read started are the first of them.
  struct Lane {
    std::deque<Range> ranges;
    // How many ranges at the front have a read started, and their bytes.
    size_t started{0};
    ULONGLONG started_bytes{0};
  };

  // Opens the handle on first use. False when it cannot be opened, now or
  // earlier: no read ahead happens then.
  [[nodiscard]] bool EnsureHandle();

  // Whether a range of length bytes at offset may be announced: its aligned
  // read neither wraps nor outgrows the cap on one range.
  [[nodiscard]] bool Admissible(ULONGLONG offset, ULONGLONG length) const;

  // Starts the reads of the next ranges, within the caps: the first queue
  // first, then the standing lane.
  void StartReads();

  // Starts the reads of the standing lane, in its order, under the caps and the
  // share of reads in flight it may take.
  void StartStandingReads();

  // Reads of the standing lane that are started and not finished. A finished
  // read waits for its caller in its slot and costs only its bytes. It visits
  // the reads in flight, not every range the lane has started.
  [[nodiscard]] size_t StandingInFlight();

#ifndef NDEBUG
  // The same count, by visiting every range the lane has started. It checks
  // StandingInFlight() in a debug build.
  [[nodiscard]] size_t StandingInFlightByScan() const;
#endif

  // Starts the read of range in a free slot. A read that cannot start leaves
  // the slot with nothing read.
  void StartRead(Range& range, Bench::Detail::DiskReadOrigin origin);

  // Waits for the read of slot, and returns the bytes it read.
  DWORD Wait(Slot& slot);

  // Forgets the range of lane at index, and cancels its read.
  void Drop(Lane& lane, size_t index);

  // The index of the first of the first count ranges of lane that holds
  // [offset, end), or count when none does.
  [[nodiscard]] static size_t FindHolding(const Lane& lane, size_t count,
                                          ULONGLONG offset, ULONGLONG end);

  // Serves a read from the range of lane at index, which holds it. The ranges
  // of the first queue before it are dropped when passed_over is set.
  [[nodiscard]] bool ServeFrom(Lane& lane, size_t index, bool passed_over,
                               ULONGLONG offset, std::span<BYTE> dest);

  // What a read finds on arrival at its range, to tell the caller's wait from
  // the CPU's.
  struct Arrival {
    // The read of the range was in flight, or not started.
    bool had_to_wait{false};
    // Announced ranges were held back by the caps.
    bool window_held_back{false};
    // The reads in flight, this one included.
    std::uint32_t in_flight{0};

    [[nodiscard]] Bench::ReadClass Class() const {
      if (had_to_wait) {
        return Bench::ReadClass::Io;
      }
      return window_held_back ? Bench::ReadClass::Window
                              : Bench::ReadClass::Ready;
    }
  };

  // What the read of the range of lane at index finds. Judged before anything
  // is dropped or started.
  [[nodiscard]] Arrival JudgeArrival(const Lane& lane, size_t index) const;

  // Copies the bytes at offset out of slot, whose read returned got bytes.
  // False when the read did not return them all.
  [[nodiscard]] static bool CopyOut(const Slot& slot, DWORD got,
                                    ULONGLONG offset, std::span<BYTE> dest);

  // The bytes the aligned read of range covers.
  [[nodiscard]] static ULONGLONG AlignedBytes(const Range& range) {
    return AlignUp(range.offset + range.length) - AlignDown(range.offset);
  }

  // Starts the read of the next range of the standing lane, which takes bytes.
  void StartStandingRead(Range& next, ULONGLONG bytes);

  // Reads of the slots that are in flight and not finished.
  [[nodiscard]] std::uint32_t InFlight() const;

  // The most reads that may be in flight at once under the active caps.
  [[nodiscard]] size_t ReadsCap() const { return active_->max_reads; }

  // The most reads the standing lane may have in flight: the first queue always
  // finds its share of the reads free, whatever the lane holds.
  [[nodiscard]] size_t StandingCap() const {
    const size_t cap = ReadsCap();
    const size_t reserve =
        (std::max)(size_t{1}, cap / first_queue_reserve_divisor);
    return cap > reserve ? cap - reserve : 0;
  }

  std::wstring path_;
  HandlePtr handle_{INVALID_HANDLE_VALUE, &CloseHandle};
  bool open_failed_{false};

  ReaderCaps caps_;
  // The caps of the read pattern in force: an element of caps_.
  const Tuning::ReaderTuning* active_{caps_.data()};

  Lane first_;
  Lane standing_;
  // The slots of the lane's reads that were started and not seen finished, at
  // most StandingCap() of them. A read leaves it when it is seen finished or
  // waited for, and when its range is dropped, before its slot can be taken by
  // another read.
  std::vector<size_t> standing_reading_;
  // A deque, so that a slot with a read in flight never moves when one is
  // added. It starts with one slot for each read in flight and grows by the
  // finished reads of the standing lane that wait for their caller.
  std::deque<Slot> slots_;
};

Win32DiskReader::Prefetcher::~Prefetcher() {
  // The lane first: a read of the first queue that is dropped must not start
  // one of the lane's in its place.
  while (!standing_.ranges.empty()) {
    Drop(standing_, 0);
  }
  Cancel();
  for (Slot& slot : slots_) {
    if (slot.event != nullptr) {
      CloseHandle(slot.event);
    }
    if (slot.buffer != nullptr) {
      VirtualFree(slot.buffer, 0, MEM_RELEASE);
    }
  }
}

bool Win32DiskReader::Prefetcher::EnsureHandle() {
  if (handle_.get() != INVALID_HANDLE_VALUE) {
    return true;
  }
  if (open_failed_) {
    return false;
  }

  // Buffered, like the main handle. An unbuffered read of a mounted volume
  // returns what is on the disk, which can lag what NTFS caches by hours.
  handle_.reset(CreateFileW(path_.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_READONLY | FILE_FLAG_OVERLAPPED |
                                FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr));
  if (handle_.get() == INVALID_HANDLE_VALUE) {
    open_failed_ = true;
    Log::Debug("No read ahead: the asynchronous handle cannot be opened");
    return false;
  }
  return true;
}

bool Win32DiskReader::Prefetcher::Admissible(ULONGLONG offset,
                                             ULONGLONG length) const {
  return offset <= (std::numeric_limits<ULONGLONG>::max)() - read_alignment &&
         length <= (std::numeric_limits<ULONGLONG>::max)() - read_alignment -
                       offset &&
         AlignUp(offset + length) - AlignDown(offset) <=
             active_->max_range_bytes;
}

void Win32DiskReader::Prefetcher::Announce(ULONGLONG offset, ULONGLONG length) {
  if (length == 0 || first_.ranges.size() >= active_->max_announced_ranges) {
    return;
  }
  // The aligned read MUST NOT wrap, and MUST fit the cap on its own.
  if (!Admissible(offset, length)) {
    return;
  }
  // The lane reads it already: a second read would only cost the disk.
  if (std::ranges::any_of(standing_.ranges, [&](const Range& held) {
        return held.offset == offset && held.length == length;
      })) {
    return;
  }
  if (!EnsureHandle()) {
    return;
  }

  first_.ranges.push_back({.offset = offset, .length = length, .slot = {}});
  StartReads();
}

void Win32DiskReader::Prefetcher::AnnounceStanding(ULONGLONG offset,
                                                   ULONGLONG length) {
  if (length == 0 || standing_.ranges.size() >= active_->max_announced_ranges ||
      !Admissible(offset, length) || !EnsureHandle()) {
    return;
  }

  standing_.ranges.push_back({.offset = offset, .length = length, .slot = {}});
  StartReads();
}

void Win32DiskReader::Prefetcher::StartReads() {
  const size_t lane_in_flight = StandingInFlight();
  while (first_.started < first_.ranges.size() &&
         first_.started + lane_in_flight < ReadsCap()) {
    Range& next = first_.ranges.at(first_.started);
    const ULONGLONG bytes = AlignedBytes(next);
    // The first one always starts: the caller is about to wait for it.
    if (first_.started != 0 &&
        first_.started_bytes + standing_.started_bytes + bytes >
            active_->max_bytes) {
      break;
    }
    StartRead(next, Bench::Detail::DiskReadOrigin::ReadAhead);
    first_.started++;
    first_.started_bytes += bytes;
  }
  StartStandingReads();
}

size_t Win32DiskReader::Prefetcher::StandingInFlight() {
#ifndef NDEBUG
  // A read finishes and never starts again, so the count only falls while
  // this runs: it lies between the two visits.
  const size_t before = StandingInFlightByScan();
#endif
  // A finished read never reverts to in flight, so it leaves for good.
  std::erase_if(standing_reading_, [this](size_t held) {
    const Slot& slot = slots_.at(held);
    return !slot.pending || ReadDone(slot.overlapped);
  });
#ifndef NDEBUG
  const size_t after = StandingInFlightByScan();
  assert(after <= standing_reading_.size() &&
         standing_reading_.size() <= before);
#endif
  return standing_reading_.size();
}

#ifndef NDEBUG
size_t Win32DiskReader::Prefetcher::StandingInFlightByScan() const {
  size_t reading = 0;
  for (size_t index = 0; index < standing_.started; index++) {
    const std::optional<size_t>& held = standing_.ranges.at(index).slot;
    if (held) {
      const Slot& slot = slots_.at(*held);
      if (slot.pending && !ReadDone(slot.overlapped)) {
        reading++;
      }
    }
  }
  return reading;
}
#endif

void Win32DiskReader::Prefetcher::StartStandingReads() {
  size_t in_flight = StandingInFlight();
  while (standing_.started < standing_.ranges.size() &&
         in_flight < StandingCap() && first_.started + in_flight < ReadsCap()) {
    Range& next = standing_.ranges.at(standing_.started);
    const ULONGLONG bytes = AlignedBytes(next);
    // The lane stops at the first range that does not fit, so that the reads
    // start in the order of the announcements. Alone, a range always fits.
    if (first_.started_bytes + standing_.started_bytes != 0 &&
        first_.started_bytes + standing_.started_bytes + bytes >
            active_->max_bytes) {
      return;
    }
    StartStandingRead(next, bytes);
    in_flight++;
  }
}

void Win32DiskReader::Prefetcher::StartStandingRead(Range& next,
                                                    ULONGLONG bytes) {
  StartRead(next, Bench::Detail::DiskReadOrigin::Standing);
  if (slots_.at(*next.slot).pending) {
    standing_reading_.push_back(*next.slot);
  }
  standing_.started++;
  standing_.started_bytes += bytes;
}

void Win32DiskReader::Prefetcher::StartRead(
    Range& range, Bench::Detail::DiskReadOrigin origin) {
  // Each started range holds one slot, finished or not.
  auto free = std::ranges::find_if(
      slots_, [](const Slot& slot) { return !slot.in_use; });
  if (free == slots_.end()) {
    slots_.emplace_back();
    free = std::prev(slots_.end());
  }
  Slot& slot = *free;
  range.slot = static_cast<size_t>(free - slots_.begin());
  slot.in_use = true;
  slot.pending = false;
  slot.got = 0;

  slot.start = AlignDown(range.offset);
  const auto bytes =
      gsl::narrow<size_t>(AlignUp(range.offset + range.length) - slot.start);
  if (slot.capacity < bytes) {
    if (slot.buffer != nullptr) {
      VirtualFree(slot.buffer, 0, MEM_RELEASE);
    }
    slot.buffer = static_cast<BYTE*>(
        VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    slot.capacity = slot.buffer != nullptr ? bytes : 0;
    if (slot.buffer == nullptr) {
      return;
    }
  }
  if (slot.event == nullptr) {
    slot.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (slot.event == nullptr) {
      return;
    }
  }

  slot.overlapped = OVERLAPPED{};
  slot.overlapped.hEvent = slot.event;
  slot.overlapped.Offset = static_cast<DWORD>(slot.start);
  slot.overlapped.OffsetHigh = static_cast<DWORD>(slot.start >> 32U);
  const std::uint32_t in_flight =
      Bench::Detail::SessionOn() ? InFlight() + 1 : 0;
  // A read that completes at once still reports through the OVERLAPPED.
  slot.pending = ReadFile(handle_.get(), slot.buffer, gsl::narrow<DWORD>(bytes),
                          nullptr, &slot.overlapped) != FALSE ||
                 GetLastError() == ERROR_IO_PENDING;
  if (slot.pending) {
    Bench::Detail::NoteDiskRead(slot.start, bytes, origin, in_flight);
  }
}

std::uint32_t Win32DiskReader::Prefetcher::InFlight() const {
  std::uint32_t reading = 0;
  for (const Slot& slot : slots_) {
    if (slot.in_use && slot.pending && !ReadDone(slot.overlapped)) {
      reading++;
    }
  }
  return reading;
}

void Win32DiskReader::Prefetcher::SetPattern(Tuning::Pattern pattern) {
  active_ = &caps_.at(static_cast<size_t>(pattern));
  StartReads();
}

DWORD Win32DiskReader::Prefetcher::Wait(Slot& slot) {
  if (slot.pending) {
    DWORD got = 0;
    slot.got = GetOverlappedResult(handle_.get(), &slot.overlapped, &got,
                                   TRUE) != FALSE
                   ? got
                   : 0;
    slot.pending = false;
  }
  return slot.got;
}

void Win32DiskReader::Prefetcher::Drop(Lane& lane, size_t index) {
  const Range& range = lane.ranges.at(index);
  if (range.slot) {
    Slot& slot = slots_.at(*range.slot);
    if (slot.pending) {
      // The buffer is free only once the read is over, cancelled or not.
      CancelIoEx(handle_.get(), &slot.overlapped);
      static_cast<void>(Wait(slot));
    }
    slot.in_use = false;
    if (&lane == &standing_) {
      // Before the slot is free for another read.
      std::erase(standing_reading_, *range.slot);
    }
    if (&lane == &standing_ && slot.capacity > lane_buffer_keep_bytes) {
      VirtualFree(slot.buffer, 0, MEM_RELEASE);
      slot.buffer = nullptr;
      slot.capacity = 0;
    }
    const ULONGLONG bytes = AlignedBytes(range);
    lane.started--;
    lane.started_bytes -= bytes;
    if (!range.used) {
      Bench::Detail::NoteDiskWaste(bytes, &lane == &standing_);
    }
  }
  lane.ranges.erase(lane.ranges.begin() + static_cast<std::ptrdiff_t>(index));
}

size_t Win32DiskReader::Prefetcher::FindHolding(const Lane& lane, size_t count,
                                                ULONGLONG offset,
                                                ULONGLONG end) {
  size_t match = 0;
  while (match < count &&
         (offset < lane.ranges.at(match).offset ||
          end > lane.ranges.at(match).offset + lane.ranges.at(match).length)) {
    match++;
  }
  return match;
}

bool Win32DiskReader::Prefetcher::Serve(ULONGLONG offset,
                                        std::span<BYTE> dest) {
  if (dest.empty()) {
    return false;
  }

  const ULONGLONG end = offset + dest.size();
  const size_t searched =
      (std::min)(first_.ranges.size(), active_->match_lookahead);
  const size_t match = FindHolding(first_, searched, offset, end);
  if (match < searched) {
    return ServeFrom(first_, match, true, offset, dest);
  }
  // A range of the lane is never passed over: it is used up, or withdrawn.
  const size_t held =
      FindHolding(standing_, standing_.ranges.size(), offset, end);
  if (held < standing_.ranges.size()) {
    return ServeFrom(standing_, held, false, offset, dest);
  }
  return false;
}

Win32DiskReader::Prefetcher::Arrival
    Win32DiskReader::Prefetcher::JudgeArrival(const Lane& lane,
                                              size_t index) const {
  const Range& matched = lane.ranges.at(index);
  return {.had_to_wait =
              !matched.slot || (slots_.at(*matched.slot).pending &&
                                !ReadDone(slots_.at(*matched.slot).overlapped)),
          .window_held_back = lane.started < lane.ranges.size(),
          .in_flight = InFlight() + 1};
}

bool Win32DiskReader::Prefetcher::CopyOut(const Slot& slot, DWORD got,
                                          ULONGLONG offset,
                                          std::span<BYTE> dest) {
  const ULONGLONG skip = offset - slot.start;
  if (skip + dest.size() > got) {
    return false;
  }
  std::ranges::copy(std::span<const BYTE>{slot.buffer, slot.capacity}.subspan(
                        gsl::narrow<size_t>(skip), dest.size()),
                    dest.begin());
  return true;
}

bool Win32DiskReader::Prefetcher::ServeFrom(Lane& lane, size_t index,
                                            bool passed_over, ULONGLONG offset,
                                            std::span<BYTE> dest) {
  const ULONGLONG end = offset + dest.size();

  const bool bench = Bench::Detail::SessionOn();
  const Arrival arrival = bench ? JudgeArrival(lane, index) : Arrival{};

  // The ranges of the first queue announced before it were passed over.
  if (passed_over) {
    for (size_t dropped = 0; dropped < index; dropped++) {
      Drop(lane, 0);
    }
    index = 0;
  }
  StartReads();

  Range& range = lane.ranges.at(index);
  bool served = false;
  std::uint64_t wait_ns = 0;
  if (range.slot) {
    Slot& slot = slots_.at(*range.slot);
    const std::uint64_t waited_from = bench ? Bench::Detail::NowNs() : 0;
    const DWORD got = Wait(slot);
    if (bench && arrival.had_to_wait) {
      wait_ns = Bench::Detail::NowNs() - waited_from;
    }
    served = CopyOut(slot, got, offset, dest);
    range.used = range.used || served;
  }
  if (served && bench) {
    Bench::Detail::NoteRead(arrival.Class(), wait_ns, arrival.in_flight);
  }

  // A read up to the end of its range uses it up. A failed one is not tried
  // again: the caller's own reads report the failure.
  if (!served || end == range.offset + range.length) {
    Drop(lane, index);
  }
  StartReads();
  return served;
}

void Win32DiskReader::Prefetcher::Cancel() {
  while (!first_.ranges.empty()) {
    Drop(first_, 0);
  }
  // The slots that were freed go to the lane.
  StartReads();
}

void Win32DiskReader::Prefetcher::CancelStanding(ULONGLONG offset,
                                                 ULONGLONG length) {
  constexpr ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
  if (length == 0) {
    return;
  }
  // An interval that runs past the largest offset is cut there.
  const ULONGLONG end =
      length > max_value - offset ? max_value : offset + length;
  size_t index = 0;
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

Win32DiskReader::Win32DiskReader(const ReaderCaps& caps)
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle)), caps_(caps) {}

Win32DiskReader::~Win32DiskReader() = default;

bool Win32DiskReader::Open(std::wstring_view path) {
  // What was read ahead belongs to the previous store.
  prefetcher_.reset();
  media_kind_ = Tuning::MediaKind::Unknown;

  // Extended, so a path of more than 260 units opens.
  std::wstring path_z = ToExtendedLengthPath(path);
  // SEQUENTIAL_SCAN is only a hint: it asks the cache manager for more
  // read-ahead and earlier eviction of pages already read, so an $MFT scan
  // does not flood the system cache. A random read stays correct.
  handle_ = HandlePtr(
      CreateFileW(path_z.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_READONLY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr),
      &CloseHandle);
  if (handle_.get() == INVALID_HANDLE_VALUE) {
    return false;
  }

  media_kind_ = SeekPenaltyKindOf(handle_.get());
  // A plain file refuses the query: its volume answers for it.
  if (ask_host_volume_ && media_kind_ == Tuning::MediaKind::Unknown &&
      IsPlainFile(handle_.get())) {
    media_kind_ = QueryHostMediaKind(path_z);
  }
  prefetcher_ = std::make_unique<Prefetcher>(std::move(path_z), caps_);
  return true;
}

Tuning::MediaKind Win32DiskReader::GetMediaKind() const { return media_kind_; }

bool Win32DiskReader::ReadInto(LARGE_INTEGER& addr,
                               std::span<BYTE> dest) const {
  // OVERLAPPED carries the offset as unsigned: a negative one would wrap.
  if (addr.QuadPart < 0) {
    Log::Error("Cannot read file at adress {}: negative address",
               addr.QuadPart);
    return false;
  }

  if (prefetcher_ != nullptr &&
      prefetcher_->Serve(static_cast<ULONGLONG>(addr.QuadPart), dest)) {
    return true;
  }

  // The handle is synchronous, so OVERLAPPED only carries the offset. ReadFile
  // reads there and ignores the handle's own file pointer.
  OVERLAPPED overlapped{};
  overlapped.Offset = addr.LowPart;
  overlapped.OffsetHigh = gsl::narrow<DWORD>(addr.HighPart);

  const bool bench = Bench::Detail::SessionOn();
  const std::uint64_t began = bench ? Bench::Detail::NowNs() : 0;
  DWORD len = 0;
  const bool read =
      ReadFile(handle_.get(), dest.data(), gsl::narrow<DWORD>(dest.size()),
               &len, &overlapped) != FALSE &&
      len == dest.size();
  if (bench) {
    // The caller waited for the whole read: nothing announced it.
    const std::uint32_t in_flight =
        prefetcher_ != nullptr ? prefetcher_->InFlightReads() + 1 : 1;
    Bench::Detail::NoteDiskRead(
        static_cast<std::uint64_t>(addr.QuadPart), dest.size(),
        Bench::Detail::DiskReadOrigin::Synchronous, in_flight);
    Bench::Detail::NoteRead(Bench::ReadClass::Plan,
                            Bench::Detail::NowNs() - began, in_flight);
  }
  if (!read) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return false;
  }

  return true;
}

std::optional<ULONGLONG> Win32DiskReader::Size() const {
  // Answers for a disk or a volume handle, and fails for an ordinary file.
  GET_LENGTH_INFORMATION length_info{};
  DWORD returned = 0;
  if (DeviceIoControl(handle_.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                      &length_info, sizeof(length_info), &returned,
                      nullptr) != FALSE &&
      length_info.Length.QuadPart >= 0) {
    return gsl::narrow<ULONGLONG>(length_info.Length.QuadPart);
  }

  LARGE_INTEGER file_size{};
  if (GetFileSizeEx(handle_.get(), &file_size) != FALSE &&
      file_size.QuadPart >= 0) {
    return gsl::narrow<ULONGLONG>(file_size.QuadPart);
  }
  return std::nullopt;
}

void Win32DiskReader::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->Announce(offset, length);
  }
}

void Win32DiskReader::CancelPrefetch() const {
  if (prefetcher_ != nullptr) {
    prefetcher_->Cancel();
  }
}

void Win32DiskReader::PrefetchStanding(ULONGLONG offset,
                                       ULONGLONG length) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->AnnounceStanding(offset, length);
  }
}

void Win32DiskReader::CancelStanding(ULONGLONG offset, ULONGLONG length) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->CancelStanding(offset, length);
  }
}

void Win32DiskReader::SetReadPattern(Tuning::Pattern pattern) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->SetPattern(pattern);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
