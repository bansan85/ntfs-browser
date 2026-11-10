#include "io/async-read-pool.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// The largest buffer that a slot keeps once the standing lane is done with it.
// The lane holds up to a hundred slots at a time, so a slot MUST NOT keep the
// buffer of one big range for the small ranges that follow. 64 KiB is the
// widest run that the probes of a directory read in the usual case.
constexpr std::size_t lane_buffer_keep_bytes = 64 * 1024;

// The probe a test installed in place of the system's answer, or null.
std::atomic<ReadDoneProbe> read_done_probe{nullptr};

// Whether the read behind overlapped is over. It never reverts: a read that is
// over stays over until its slot starts another.
bool ReadDone(const OVERLAPPED& overlapped) noexcept {
  const ReadDoneProbe probe = read_done_probe.load(std::memory_order_relaxed);
  return probe != nullptr ? probe(overlapped)
                          : HasOverlappedIoCompleted(&overlapped);
}

}  // namespace

ReadDoneProbe SetReadDoneProbeForTests(ReadDoneProbe probe) noexcept {
  return read_done_probe.exchange(probe);
}

AsyncReadPool::AsyncReadPool(std::wstring path, std::size_t slots)
    : path_(std::move(path)), slots_(slots) {}

AsyncReadPool::~AsyncReadPool() {
  for (Slot& slot : slots_) {
    if (slot.event != nullptr) {
      CloseHandle(slot.event);
    }
    if (slot.buffer != nullptr) {
      VirtualFree(slot.buffer, 0, MEM_RELEASE);
    }
  }
}

bool AsyncReadPool::EnsureOpen() {
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

std::size_t AsyncReadPool::Acquire() {
  // Each started range holds one slot, finished or not.
  auto free = std::ranges::find_if(
      slots_, [](const Slot& slot) { return !slot.in_use; });
  if (free == slots_.end()) {
    slots_.emplace_back();
    free = std::prev(slots_.end());
  }
  free->in_use = true;
  free->pending = false;
  free->got = 0;
  return static_cast<std::size_t>(free - slots_.begin());
}

bool AsyncReadPool::Prepare(Slot& slot, std::size_t bytes) {
  if (slot.capacity < bytes) {
    if (slot.buffer != nullptr) {
      VirtualFree(slot.buffer, 0, MEM_RELEASE);
    }
    slot.buffer = static_cast<BYTE*>(
        VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    slot.capacity = slot.buffer != nullptr ? bytes : 0;
    if (slot.buffer == nullptr) {
      return false;
    }
  }
  if (slot.event == nullptr) {
    slot.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  return slot.event != nullptr;
}

void AsyncReadPool::Start(std::size_t slot_number, ULONGLONG start,
                          std::size_t bytes,
                          Bench::Detail::DiskReadOrigin origin) {
  Slot& slot = slots_.at(slot_number);
  slot.start = start;
  if (!Prepare(slot, bytes)) {
    return;
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

DWORD AsyncReadPool::Wait(std::size_t slot_number) {
  Slot& slot = slots_.at(slot_number);
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

void AsyncReadPool::Release(std::size_t slot_number, bool shrink) {
  Slot& slot = slots_.at(slot_number);
  if (slot.pending) {
    // The buffer is free only once the read is over, cancelled or not.
    CancelIoEx(handle_.get(), &slot.overlapped);
    static_cast<void>(Wait(slot_number));
  }
  slot.in_use = false;
  if (shrink && slot.capacity > lane_buffer_keep_bytes) {
    VirtualFree(slot.buffer, 0, MEM_RELEASE);
    slot.buffer = nullptr;
    slot.capacity = 0;
  }
}

bool AsyncReadPool::Reading(std::size_t slot_number) const {
  const Slot& slot = slots_.at(slot_number);
  return slot.pending && !ReadDone(slot.overlapped);
}

bool AsyncReadPool::Pending(std::size_t slot_number) const {
  return slots_.at(slot_number).pending;
}

std::uint32_t AsyncReadPool::InFlight() const {
  std::uint32_t reading = 0;
  for (const Slot& slot : slots_) {
    if (slot.in_use && slot.pending && !ReadDone(slot.overlapped)) {
      reading++;
    }
  }
  return reading;
}

bool AsyncReadPool::CopyOut(std::size_t slot_number, DWORD got,
                            ULONGLONG offset, std::span<BYTE> dest) const {
  const Slot& slot = slots_.at(slot_number);
  const ULONGLONG skip = offset - slot.start;
  if (skip + dest.size() > got) {
    return false;
  }
  std::ranges::copy(std::span<const BYTE>{slot.buffer, slot.capacity}.subspan(
                        gsl::narrow<std::size_t>(skip), dest.size()),
                    dest.begin());
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
