#include "io/win32-disk-reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
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

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "bench/recorder.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Alignment of a read ahead, in offset and length. A device takes only reads
// of whole sectors: this is a multiple of both sizes in use, 512 and 4096.
constexpr ULONGLONG read_alignment = 4096;

// The offset of the first byte of an aligned read that covers offset.
constexpr ULONGLONG AlignDown(ULONGLONG offset) noexcept {
  return offset - offset % read_alignment;
}

// The end of an aligned read that covers the byte before end. end MUST be at
// most read_alignment below the largest ULONGLONG.
constexpr ULONGLONG AlignUp(ULONGLONG end) noexcept {
  return AlignDown(end + read_alignment - 1);
}

}  // namespace

// Reads the announced ranges ahead, in announcement order, through an
// overlapped handle of its own. Up to ReaderTuning::max_reads of them are in
// flight at once, so the disk reads them while the caller works. Every call
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

  void Announce(ULONGLONG offset, ULONGLONG length);

  // Copies the bytes at offset into dest when an announced range holds them
  // all and its read succeeded. False otherwise: the caller then reads them
  // itself, and reports a failure itself.
  [[nodiscard]] bool Serve(ULONGLONG offset, std::span<BYTE> dest);

  void Cancel();

  // Reads of the slots in flight and not finished. Only a session needs it.
  [[nodiscard]] std::uint32_t InFlightReads() const { return InFlight(); }

  // Switches the caps to those of pattern. The reads in flight finish whatever
  // the new caps say, and the ranges waiting start under them.
  void SetPattern(Tuning::Pattern pattern);

 private:
  // The most reads in flight any pattern allows: the slots are sized once, so
  // that a read in flight never moves.
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

  // Opens the handle on first use. False when it cannot be opened, now or
  // earlier: no read ahead happens then.
  [[nodiscard]] bool EnsureHandle();

  // Starts the reads of the next ranges, within the caps.
  void StartReads();

  // Starts the read of range in a free slot. A read that cannot start leaves
  // the slot with nothing read.
  void StartRead(Range& range);

  // Waits for the read of slot, and returns the bytes it read.
  DWORD Wait(Slot& slot);

  // Forgets the first range, and cancels its read.
  void DropFront();

  // Reads of the slots that are in flight and not finished.
  [[nodiscard]] std::uint32_t InFlight() const;

  // The most reads that may be started at once under the active caps.
  [[nodiscard]] size_t ReadsCap() const {
    return (std::min)(active_->max_reads, slots_.size());
  }

  std::wstring path_;
  HandlePtr handle_{INVALID_HANDLE_VALUE, &CloseHandle};
  bool open_failed_{false};

  ReaderCaps caps_;
  // The caps of the read pattern in force: an element of caps_.
  const Tuning::ReaderTuning* active_{caps_.data()};

  std::deque<Range> ranges_;
  // The ranges at the front of ranges_ whose read started, and their bytes.
  size_t started_{0};
  ULONGLONG started_bytes_{0};
  std::vector<Slot> slots_;
};

Win32DiskReader::Prefetcher::~Prefetcher() {
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

void Win32DiskReader::Prefetcher::Announce(ULONGLONG offset, ULONGLONG length) {
  if (length == 0 || ranges_.size() >= active_->max_announced_ranges) {
    return;
  }
  // The aligned read MUST NOT wrap, and MUST fit the cap on its own.
  if (offset > (std::numeric_limits<ULONGLONG>::max)() - read_alignment ||
      length >
          (std::numeric_limits<ULONGLONG>::max)() - read_alignment - offset ||
      AlignUp(offset + length) - AlignDown(offset) > active_->max_range_bytes) {
    return;
  }
  if (!EnsureHandle()) {
    return;
  }

  ranges_.push_back({.offset = offset, .length = length, .slot = {}});
  StartReads();
}

void Win32DiskReader::Prefetcher::StartReads() {
  while (started_ < ranges_.size() && started_ < ReadsCap()) {
    Range& next = ranges_.at(started_);
    const ULONGLONG bytes =
        AlignUp(next.offset + next.length) - AlignDown(next.offset);
    // The first one always starts: the caller is about to wait for it.
    if (started_ != 0 && started_bytes_ + bytes > active_->max_bytes) {
      return;
    }
    StartRead(next);
    started_++;
    started_bytes_ += bytes;
  }
}

void Win32DiskReader::Prefetcher::StartRead(Range& range) {
  // started_ < ReadsCap(), and each started range holds one slot.
  const auto free = std::ranges::find_if(
      slots_, [](const Slot& slot) { return !slot.in_use; });
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
    Bench::Detail::NoteDiskRead(slot.start, bytes, true, in_flight);
  }
}

std::uint32_t Win32DiskReader::Prefetcher::InFlight() const {
  std::uint32_t reading = 0;
  for (const Slot& slot : slots_) {
    if (slot.in_use && slot.pending &&
        !HasOverlappedIoCompleted(&slot.overlapped)) {
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

void Win32DiskReader::Prefetcher::DropFront() {
  const Range& front = ranges_.front();
  if (front.slot) {
    Slot& slot = slots_.at(*front.slot);
    if (slot.pending) {
      // The buffer is free only once the read is over, cancelled or not.
      CancelIoEx(handle_.get(), &slot.overlapped);
      static_cast<void>(Wait(slot));
    }
    slot.in_use = false;
    const ULONGLONG bytes =
        AlignUp(front.offset + front.length) - AlignDown(front.offset);
    started_--;
    started_bytes_ -= bytes;
    if (!front.used) {
      Bench::Detail::NoteDiskWaste(bytes);
    }
  }
  ranges_.pop_front();
}

bool Win32DiskReader::Prefetcher::Serve(ULONGLONG offset,
                                        std::span<BYTE> dest) {
  if (dest.empty()) {
    return false;
  }

  const ULONGLONG end = offset + dest.size();
  const size_t searched = (std::min)(ranges_.size(), active_->match_lookahead);
  size_t match = 0;
  while (match < searched &&
         (offset < ranges_.at(match).offset ||
          end > ranges_.at(match).offset + ranges_.at(match).length)) {
    match++;
  }
  if (match == searched) {
    return false;
  }

  // What the read finds on arrival, to tell the caller's wait from the CPU's.
  // Judged before anything is dropped or started.
  const bool bench = Bench::Detail::SessionOn();
  bool had_to_wait = false;
  bool window_held_back = false;
  std::uint32_t in_flight = 0;
  if (bench) {
    const Range& matched = ranges_.at(match);
    had_to_wait =
        !matched.slot ||
        (slots_.at(*matched.slot).pending &&
         !HasOverlappedIoCompleted(&slots_.at(*matched.slot).overlapped));
    window_held_back = started_ < ranges_.size();
    in_flight = InFlight() + 1;
  }

  // The ranges announced before it were passed over.
  for (size_t dropped = 0; dropped < match; dropped++) {
    DropFront();
  }
  StartReads();

  Range& range = ranges_.front();
  bool served = false;
  std::uint64_t wait_ns = 0;
  if (range.slot) {
    Slot& slot = slots_.at(*range.slot);
    const ULONGLONG skip = offset - slot.start;
    const std::uint64_t waited_from = bench ? Bench::Detail::NowNs() : 0;
    const DWORD got = Wait(slot);
    if (bench && had_to_wait) {
      wait_ns = Bench::Detail::NowNs() - waited_from;
    }
    if (skip + dest.size() <= got) {
      std::ranges::copy(
          std::span<const BYTE>{slot.buffer, slot.capacity}.subspan(
              gsl::narrow<size_t>(skip), dest.size()),
          dest.begin());
      served = true;
      range.used = true;
    }
  }
  if (served && bench) {
    Bench::Detail::NoteRead(had_to_wait        ? Bench::ReadClass::Io
                            : window_held_back ? Bench::ReadClass::Window
                                               : Bench::ReadClass::Ready,
                            wait_ns, in_flight);
  }

  // A read up to the end of its range uses it up. A failed one is not tried
  // again: the caller's own reads report the failure.
  if (!served || end == range.offset + range.length) {
    DropFront();
  }
  StartReads();
  return served;
}

void Win32DiskReader::Prefetcher::Cancel() {
  while (!ranges_.empty()) {
    DropFront();
  }
}

Win32DiskReader::Win32DiskReader(const ReaderCaps& caps)
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle)), caps_(caps) {}

Win32DiskReader::~Win32DiskReader() = default;

bool Win32DiskReader::Open(std::wstring_view path) {
  // What was read ahead belongs to the previous store.
  prefetcher_.reset();

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

  prefetcher_ = std::make_unique<Prefetcher>(std::move(path_z), caps_);
  return true;
}

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
    Bench::Detail::NoteDiskRead(static_cast<std::uint64_t>(addr.QuadPart),
                                dest.size(), false, in_flight);
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

void Win32DiskReader::SetReadPattern(Tuning::Pattern pattern) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->SetPattern(pattern);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
