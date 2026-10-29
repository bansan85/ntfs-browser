#pragma once

#include <ntfs-browser/win-types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "internal-export.h"

// The hooks that the read path calls. Every one is inline and starts with a
// test of one atomic flag, so that a run without a session pays a load and a
// predictable branch, and nothing else: no allocation, no lock, no clock.
namespace NTFS_BROWSER_NAMESPACE::Bench::Detail {

// Bit 0: a session runs. Bit 1: a capture sink is set. Bit 2: a stop-after
// phase is set.
inline constexpr unsigned flag_session = 1U;
inline constexpr unsigned flag_capture = 2U;
inline constexpr unsigned flag_stop = 4U;

NTFS_BROWSER_EXPORT_TESTS_ONLY extern std::atomic<unsigned> g_flags;

// Whether a session runs, or a capture is on.
[[nodiscard]] inline bool Active() noexcept {
  return (g_flags.load(std::memory_order_relaxed) &
          (flag_session | flag_capture)) != 0;
}

// Whether a session runs.
[[nodiscard]] inline bool SessionOn() noexcept {
  return (g_flags.load(std::memory_order_relaxed) & flag_session) != 0;
}

// Nanoseconds since the session started.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::uint64_t NowNs() noexcept;

// CPU time, user plus kernel, of the calling thread, in nanoseconds.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::uint64_t
    ThreadCpuNs() noexcept;

// What the reader noted about the read that just ran on this thread.
struct ReadNote {
  ReadClass cls{ReadClass::Unclassified};
  std::uint64_t wait_ns{0};
  std::uint32_t queue_depth{0};
  // True when the reader noted something since ResetReadNote().
  bool valid{false};
};

// Forgets the note of the previous read. FileReader calls it before a read.
NTFS_BROWSER_EXPORT_TESTS_ONLY void ResetReadNote() noexcept;

// What the reader noted since ResetReadNote().
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY const ReadNote&
    LastReadNote() noexcept;

// The reader says how it served the read it is serving, and how long the
// caller waited.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    NoteRead(ReadClass cls, std::uint64_t wait_ns,
             std::uint32_t queue_depth) noexcept;

// The reader says it issued a read to the disk: a read ahead or a synchronous
// one. bytes is what the disk is asked for, queue_depth the reads in flight
// with it.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    NoteDiskRead(std::uint64_t offset, std::uint64_t bytes, bool prefetch,
                 std::uint32_t queue_depth) noexcept;

// The reader says a read ahead of bytes was dropped before any read used it.
NTFS_BROWSER_EXPORT_TESTS_ONLY void NoteDiskWaste(std::uint64_t bytes) noexcept;

// FileReader says a ReadInto() of the volume ran. ok is its verdict.
NTFS_BROWSER_EXPORT_TESTS_ONLY void RecordRead(std::uint64_t offset,
                                               std::uint64_t length,
                                               std::uint64_t start_ns,
                                               std::uint64_t end_ns) noexcept;

NTFS_BROWSER_EXPORT_TESTS_ONLY void
    RecordPrefetch(std::uint64_t offset, std::uint64_t length) noexcept;
NTFS_BROWSER_EXPORT_TESTS_ONLY void RecordCancel() noexcept;
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    RecordPattern(Tuning::Pattern pattern) noexcept;

// FileReader says the volume has this many bytes, when it knows.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    NoteVolumeBytes(std::uint64_t bytes) noexcept;

// Hands the bytes of a successful read to the capture sink, if one is set.
NTFS_BROWSER_EXPORT_TESTS_ONLY void CaptureRead(std::uint64_t offset,
                                                std::span<const BYTE> bytes);

// The kind the next reads of this thread are for.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY ReadKind CurrentKind() noexcept;
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetCurrentKind(ReadKind kind) noexcept;

// Declares what the reads made while it lives are for. With only_if_other, it
// leaves a kind that an outer scope already set: a general scope (file data)
// then yields to a specific one (a bitmap window).
class KindScope {
 public:
  explicit KindScope(ReadKind kind, bool only_if_other = false) noexcept
      : active_(SessionOn()) {
    if (active_) {
      previous_ = CurrentKind();
      if (!only_if_other || previous_ == ReadKind::Other) {
        SetCurrentKind(kind);
      }
    }
  }

  KindScope(const KindScope&) = delete;
  KindScope& operator=(const KindScope&) = delete;
  KindScope(KindScope&&) = delete;
  KindScope& operator=(KindScope&&) = delete;

  ~KindScope() {
    if (active_) {
      SetCurrentKind(previous_);
    }
  }

 private:
  bool active_;
  ReadKind previous_{ReadKind::Other};
};

// Where a slice began, and what the slice enclosing it had gathered.
struct SliceMark {
  std::uint64_t begin_ns{0};
  std::uint64_t wait_ns{0};
  std::uint64_t saved_child_ns{0};
  std::uint64_t saved_child_wait{0};
};

NTFS_BROWSER_EXPORT_TESTS_ONLY SliceMark SliceBegin() noexcept;
NTFS_BROWSER_EXPORT_TESTS_ONLY void SliceEnd(Slice slice,
                                             const SliceMark& mark) noexcept;

// Times a slice of the main thread's work, if a session runs. Slices nest: the
// inner one is taken out of the outer one.
class SliceScope {
 public:
  explicit SliceScope(Slice slice) noexcept
      : active_(SessionOn()), slice_(slice) {
    if (active_) {
      mark_ = SliceBegin();
    }
  }

  SliceScope(const SliceScope&) = delete;
  SliceScope& operator=(const SliceScope&) = delete;
  SliceScope(SliceScope&&) = delete;
  SliceScope& operator=(SliceScope&&) = delete;

  ~SliceScope() {
    if (active_) {
      SliceEnd(slice_, mark_);
    }
  }

 private:
  bool active_;
  Slice slice_;
  SliceMark mark_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Bench::Detail
