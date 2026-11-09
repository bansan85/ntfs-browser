#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "cache/strategy.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/held-records.h"
#include "mft/slot-entry.h"
#include "mft/slot-reader.h"
#include "util/chunk-pool.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// Parses the slots of one window on several threads, and gives the outcome of
// each in record order. The window is cut into chunks. The threads take them in
// order, each with a FileRecord of its own, and a chunk logs into a capture of
// its own, which the thread that owns the scan replays record by record.
template <Cache::Strategy S>
class WindowParser {
 public:
  // threads is the number of threads that parse, the calling one included. A
  // null bitmap parses every slot, else the slots it marks free are left alone.
  WindowParser(const NtfsVolume<S>& volume, size_t threads,
               const std::vector<BYTE>* bitmap, SlotFeatures features);

  // Starts parsing window, after the one before it was ended. The bytes of
  // window MUST stay as they are until End().
  void Begin(const Mft::SlotWindow& window);

  // Waits for the threads that still parse, and drops the chunks not started.
  void End();

  // Whether the window being parsed holds slot record.
  [[nodiscard]] bool Covers(ULONGLONG record) const noexcept;

  // The outcome of slot record, once its chunk is parsed. record MUST be a slot
  // of the window that is not marked free. The reference stays valid until the
  // next Begin().
  SlotOutcome& Take(ULONGLONG record);

  // The log that holds the lines the parse of record logged, which the
  // outcome marks. It stays valid until the second Begin() after the window.
  [[nodiscard]] const Log::Detail::CapturedLog& LogOf(ULONGLONG record) const;

 private:
  // The chunk number of no chunk.
  static constexpr size_t no_chunk = (std::numeric_limits<size_t>::max)();

  // Parses the slots of one chunk on thread slot.
  void ParseChunk(size_t chunk, size_t slot);

  const NtfsVolume<S>* volume_;
  size_t record_size_;
  // Slots one chunk holds, from the volume's tuning.
  size_t chunk_slots_;
  const std::vector<BYTE>* bitmap_;
  SlotFeatures features_;
  Mft::SlotWindow window_;
  Log::Detail::LevelMask levels_{0};
  bool active_{false};
  // The last chunk Take() waited for, so that a chunk is waited for once.
  size_t waited_chunk_{no_chunk};
  std::vector<std::unique_ptr<Io::FileRecord<S>>> contexts_;
  std::vector<SlotOutcome> outcomes_;
  // The capture of each chunk, for the window being parsed and for the one
  // before it.
  std::array<std::vector<Log::Detail::CapturedLog>, 2> logs_;
  size_t log_set_{0};
  // Last: its threads use the members above, so it ends first.
  Util::ChunkPool pool_;
};

// Parses the base records that wait for the second pass, on several threads,
// and gives the outcome of each in waiting order. A batch of waiting records is
// cut into chunks, as WindowParser does with the slots of a window: a thread
// takes a chunk at a time with a FileRecord of its own, and a chunk logs into a
// capture of its own, which the scan's thread replays record by record.
//
// A thread reads nothing of the volume. It parses a record whose bytes are held
// and whose extension records are all held. Its FileRecord refuses a read that
// a parse would make after all: an extension record not held, a non-resident
// $ATTRIBUTE_LIST or $EFS stream. The record is then left to the scan's thread,
// which parses it as the single thread does, reads included. The held records
// and the bases of the slots are not changed during the second pass, so the
// threads only read them.
template <Cache::Strategy S>
class WaitingParser {
 public:
  // threads is the number of threads that parse, the calling one included.
  // held, bases and waiting MUST NOT change while a batch is parsed.
  WaitingParser(const NtfsVolume<S>& volume, size_t threads,
                const HeldRecords& held, const SlotBases& bases,
                SlotFeatures features);

  // Starts parsing the count waiting records from first on, after the batch
  // before it was ended.
  void Begin(size_t first, size_t count);

  // Waits for the threads that still parse, and drops the chunks not started.
  void End();

  // Whether the batch being parsed holds waiting record index.
  [[nodiscard]] bool Covers(size_t index) const noexcept;

  // The outcome of waiting record index, once its chunk is parsed. index MUST
  // be covered. The reference stays valid until the next Begin().
  SlotOutcome& Take(size_t index);

  // The log that holds the lines the parse of waiting record index logged,
  // which its outcome marks. It stays valid until the second Begin() after the
  // batch.
  [[nodiscard]] const Log::Detail::CapturedLog& LogOf(size_t index) const;

 private:
  // The FileRecord of one thread, and whether a parse of it wanted a read.
  struct Context {
    explicit Context(const NtfsVolume<S>& volume);

    Io::FileRecord<S> record;
    bool refused{false};
  };

  // The chunk number of no chunk.
  static constexpr size_t no_chunk = (std::numeric_limits<size_t>::max)();

  // Parses the records of one chunk on thread slot.
  void ParseChunk(size_t chunk, size_t slot);

  // Parses waiting, whose bytes are held, as the second pass does on the
  // scan's thread: its header, then its attributes with the extension records
  // the first pass held.
  void ParseWaiting(Io::FileRecord<S>& file_record,
                    const HeldRecords::Waiting& waiting,
                    SlotOutcome& outcome) const;

  const NtfsVolume<S>* volume_;
  const HeldRecords* held_;
  const std::vector<HeldRecords::Waiting>* waiting_;
  // Waiting records one chunk holds, from the volume's tuning.
  size_t chunk_records_;
  SlotFeatures features_;
  // The batch being parsed: its first waiting record, and its length.
  size_t first_{0};
  size_t count_{0};
  Log::Detail::LevelMask levels_{0};
  bool active_{false};
  // The last chunk Take() waited for, so that a chunk is waited for once.
  size_t waited_chunk_{no_chunk};
  std::vector<std::unique_ptr<Context>> contexts_;
  std::vector<SlotOutcome> outcomes_;
  // The capture of each chunk, for the batch being parsed and for the one
  // before it.
  std::array<std::vector<Log::Detail::CapturedLog>, 2> logs_;
  size_t log_set_{0};
  // Last: its threads use the members above, so it ends first.
  Util::ChunkPool pool_;
};

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
