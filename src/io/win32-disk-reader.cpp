#include "io/win32-disk-reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gsl/narrow>
#include <winioctl.h>

#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Reads ahead in flight at once, at most. Measured on a SATA SSD, 4 KiB random
// reads run at 4 700 a second one at a time, and at 20 400 eight at a time.
constexpr size_t max_prefetch_reads = 8;

// Bytes the reads ahead hold at most, in flight or waiting to be used: four
// blocks of an $MFT scan.
constexpr ULONGLONG max_prefetch_bytes = ULONGLONG{16} * 1024 * 1024;

// Announced ranges no read has used yet, at most. Past it a hint is dropped: a
// caller that announces far ahead of its reads would only fill memory.
constexpr size_t max_announced_ranges = 1024;

// Announced ranges searched for the one a read falls in. Reads follow the
// announcements, so the match is near the front.
constexpr size_t max_match_lookahead = 64;

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
// overlapped handle of its own. Up to max_prefetch_reads of them are in flight
// at once, so the disk reads them while the caller works. Every call comes from
// the caller's thread, so nothing is locked.
class Win32DiskReader::Prefetcher {
 public:
  explicit Prefetcher(std::wstring path) : path_(std::move(path)) {}

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

 private:
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

  std::wstring path_;
  HandlePtr handle_{INVALID_HANDLE_VALUE, &CloseHandle};
  bool open_failed_{false};

  std::deque<Range> ranges_;
  // The ranges at the front of ranges_ whose read started, and their bytes.
  size_t started_{0};
  ULONGLONG started_bytes_{0};
  std::array<Slot, max_prefetch_reads> slots_{};
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
  if (length == 0 || ranges_.size() >= max_announced_ranges) {
    return;
  }
  // The aligned read MUST NOT wrap, and MUST fit the cap on its own.
  if (offset > (std::numeric_limits<ULONGLONG>::max)() - read_alignment ||
      length >
          (std::numeric_limits<ULONGLONG>::max)() - read_alignment - offset ||
      AlignUp(offset + length) - AlignDown(offset) > max_prefetch_bytes) {
    return;
  }
  if (!EnsureHandle()) {
    return;
  }

  ranges_.push_back({.offset = offset, .length = length, .slot = {}});
  StartReads();
}

void Win32DiskReader::Prefetcher::StartReads() {
  while (started_ < ranges_.size() && started_ < max_prefetch_reads) {
    Range& next = ranges_.at(started_);
    const ULONGLONG bytes =
        AlignUp(next.offset + next.length) - AlignDown(next.offset);
    // The first one always starts: the caller is about to wait for it.
    if (started_ != 0 && started_bytes_ + bytes > max_prefetch_bytes) {
      return;
    }
    StartRead(next);
    started_++;
    started_bytes_ += bytes;
  }
}

void Win32DiskReader::Prefetcher::StartRead(Range& range) {
  // started_ < max_prefetch_reads, and each started range holds one slot.
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
  // A read that completes at once still reports through the OVERLAPPED.
  slot.pending = ReadFile(handle_.get(), slot.buffer, gsl::narrow<DWORD>(bytes),
                          nullptr, &slot.overlapped) != FALSE ||
                 GetLastError() == ERROR_IO_PENDING;
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
    started_--;
    started_bytes_ -=
        AlignUp(front.offset + front.length) - AlignDown(front.offset);
  }
  ranges_.pop_front();
}

bool Win32DiskReader::Prefetcher::Serve(ULONGLONG offset,
                                        std::span<BYTE> dest) {
  if (dest.empty()) {
    return false;
  }

  const ULONGLONG end = offset + dest.size();
  const size_t searched = (std::min)(ranges_.size(), max_match_lookahead);
  size_t match = 0;
  while (match < searched &&
         (offset < ranges_.at(match).offset ||
          end > ranges_.at(match).offset + ranges_.at(match).length)) {
    match++;
  }
  if (match == searched) {
    return false;
  }

  // The ranges announced before it were passed over.
  for (size_t dropped = 0; dropped < match; dropped++) {
    DropFront();
  }
  StartReads();

  const Range& range = ranges_.front();
  bool served = false;
  if (range.slot) {
    Slot& slot = slots_.at(*range.slot);
    const ULONGLONG skip = offset - slot.start;
    if (skip + dest.size() <= Wait(slot)) {
      std::ranges::copy(
          std::span<const BYTE>{slot.buffer, slot.capacity}.subspan(
              gsl::narrow<size_t>(skip), dest.size()),
          dest.begin());
      served = true;
    }
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

Win32DiskReader::Win32DiskReader()
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle)) {}

Win32DiskReader::~Win32DiskReader() = default;

bool Win32DiskReader::Open(std::wstring_view path) {
  // What was read ahead belongs to the previous store.
  prefetcher_.reset();

  // A view carries no NUL terminator, and CreateFileW needs one.
  std::wstring path_z(path);
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

  prefetcher_ = std::make_unique<Prefetcher>(std::move(path_z));
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

  DWORD len = 0;
  if (ReadFile(handle_.get(), dest.data(), gsl::narrow<DWORD>(dest.size()),
               &len, &overlapped) == FALSE ||
      len != dest.size()) {
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

}  // namespace NTFS_BROWSER_NAMESPACE::Io
