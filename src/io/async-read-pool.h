#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <type_traits>

#include <windows.h>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// Answers whether the read behind overlapped is over.
using ReadDoneProbe = bool (*)(const OVERLAPPED& overlapped);

// Installs probe as the answer of every reader to whether a read ahead is over
// (null: ask the system, which is the default) and returns the one it replaces.
// A file in the system cache answers at once, so a test holds reads in flight
// with it. A probe MUST NOT revert: a read it called over stays over.
NTFS_BROWSER_EXPORT_TESTS_ONLY ReadDoneProbe
    SetReadDoneProbeForTests(ReadDoneProbe probe) noexcept;

// An overlapped handle of its own on a path, and the slots that hold the reads
// started through it: each slot is one OVERLAPPED, its event and its buffer.
// The slots are numbered. A slot keeps its buffer for the next read, and the
// pool grows by one slot when none is free. Not thread safe: every call comes
// from one thread.
class AsyncReadPool {
 public:
  // The pool opens path on first use, and starts with slots free slots.
  AsyncReadPool(std::wstring path, std::size_t slots);

  AsyncReadPool(const AsyncReadPool&) = delete;
  AsyncReadPool& operator=(const AsyncReadPool&) = delete;
  AsyncReadPool(AsyncReadPool&&) = delete;
  AsyncReadPool& operator=(AsyncReadPool&&) = delete;

  // Frees the events and the buffers. Every slot MUST be released first.
  ~AsyncReadPool();

  // Opens the handle on first use. False when it cannot be opened, now or
  // earlier: no read ahead happens then.
  [[nodiscard]] bool EnsureOpen();

  // Takes a free slot, which the pool grows when none is free, and returns its
  // number. The slot is held until Release().
  [[nodiscard]] std::size_t Acquire();

  // Starts the read of bytes at start, which MUST be aligned for the device,
  // in slot. A read that cannot start leaves the slot with nothing read.
  void Start(std::size_t slot, ULONGLONG start, std::size_t bytes,
             Bench::Detail::DiskReadOrigin origin);

  // Waits for the read of slot, and returns the bytes it read. 0 when it
  // failed. Asking again gives the same answer.
  [[nodiscard]] DWORD Wait(std::size_t slot);

  // Cancels and waits for the read of slot when one is in flight, then frees
  // the slot. A slot of the standing lane (shrink) lets go of a big buffer, so
  // that it MUST NOT keep it for the small ranges that follow.
  void Release(std::size_t slot, bool shrink);

  // Whether a read was started in slot and is not over.
  [[nodiscard]] bool Reading(std::size_t slot) const;

  // Whether a read was started in slot and not waited for yet. It may be over.
  [[nodiscard]] bool Pending(std::size_t slot) const;

  // Reads of the slots that are held, started and not over.
  [[nodiscard]] std::uint32_t InFlight() const;

  // Copies dest.size() bytes at offset out of slot, whose read returned got
  // bytes. False when the read did not return them all.
  [[nodiscard]] bool CopyOut(std::size_t slot, DWORD got, ULONGLONG offset,
                             std::span<BYTE> dest) const;

 private:
  // One read ahead and its buffer, kept for the next read.
  struct Slot {
    OVERLAPPED overlapped{};
    HANDLE event{nullptr};
    BYTE* buffer{nullptr};
    std::size_t capacity{0};
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

  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  // Makes the buffer and the event of slot hold a read of bytes. False when
  // either cannot be had.
  [[nodiscard]] static bool Prepare(Slot& slot, std::size_t bytes);

  std::wstring path_;
  HandlePtr handle_{INVALID_HANDLE_VALUE, &CloseHandle};
  bool open_failed_{false};
  // A deque, so that a slot with a read in flight never moves when one is
  // added.
  std::deque<Slot> slots_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
