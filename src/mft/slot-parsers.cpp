#include "mft/slot-parsers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "attr/mask.h"
#include "attr/type.h"
#include "bench/bench.h"
#include "bench/recorder.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/held-records.h"
#include "mft/slot-entry.h"
#include "mft/slot-reader.h"
#include "mft/used-slots.h"
#include "ntfs-volume.h"
#include "util/chunk-pool.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// The bytes of one record slot of a window, and its record number.
struct SlotBytes {
  ULONGLONG record;
  std::span<const BYTE> bytes;
};

// Gives outcome, whose record was just kept, the data note and the claims the
// scan asked for.
template <Cache::Strategy S>
void NoteKept(const Io::FileRecord<S>& file_record, const BaseSlot& base,
              SlotFeatures features, SlotOutcome& outcome) {
  if (features.annotate) {
    outcome.note = ComputeDataNote(file_record);
  }
  if (features.account) {
    outcome.claims = ComputeClaims(file_record, base, outcome.counts);
  }
}

// Parses slot, which is one slot of a window, into outcome. It runs on any
// thread: it never reads the volume, and it logs into the capture of the
// thread. A record that needs the volume is only marked, for the thread that
// owns the scan to parse: the key stream of an encrypted file may be
// non-resident, and the extension records of an $ATTRIBUTE_LIST are read from
// the disk. file_record MUST NOT resolve an $ATTRIBUTE_LIST.
template <Cache::Strategy S>
void ParseWindowSlot(Io::FileRecord<S>& file_record,
                     const NtfsVolume<S>& volume, const SlotBytes& slot,
                     SlotFeatures features, SlotOutcome& outcome) {
  const bool parsed = file_record.ParseFileRecord(slot.record, slot.bytes);
  outcome.kind = ClassifySlot(file_record, volume, parsed, outcome.counts,
                              outcome.header_damaged);
  if (outcome.kind == SlotKind::Unreadable) {
    return;
  }
  outcome.base_reference = file_record.GetBaseRecordReference();
  if (outcome.kind != SlotKind::Base) {
    return;
  }

  outcome.has_list = file_record.HasAttrHeader(Attr::Type::AttributeList);
  if (outcome.has_list) {
    outcome.list_extents =
        file_record.AttributeListExtents(max_announced_list_bytes);
    return;
  }
  if (file_record.HasAttrHeader(Attr::Type::LoggedUtilityStream)) {
    outcome.needs_scan_thread = true;
    return;
  }

  const BaseSlot base{.record = slot.record,
                      .header_damaged = outcome.header_damaged};
  outcome.kept =
      ReadBaseEntry(file_record, volume, base, outcome.counts, outcome.entry);
  if (outcome.kept) {
    NoteKept(file_record, base, features, outcome);
  }
}

}  // namespace

template <Cache::Strategy S>
WindowParser<S>::WindowParser(const NtfsVolume<S>& volume, size_t threads,
                              const std::vector<BYTE>* bitmap,
                              SlotFeatures features)
    : volume_(&volume),
      record_size_(volume.GetFileRecordSize()),
      chunk_slots_(
          (std::max<size_t>)(1,
                             volume.GetOptions().tuning.mft.parse_chunk_slots)),
      bitmap_(bitmap),
      features_(features),
      pool_(threads - 1) {
  contexts_.reserve(pool_.Slots());
  for (size_t slot = 0; slot < pool_.Slots(); slot++) {
    Io::FileRecord<S>& context =
        *contexts_.emplace_back(std::make_unique<Io::FileRecord<S>>(volume));
    context.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
    if (features.account) {
      context.SetClaimMask(Attr::Mask::All);
    }
    // Resolving a list reads the volume, which no worker does. A record with a
    // list never gets to parse its attributes here.
    context.SetResolveAttrList(false);
  }
}

template <Cache::Strategy S>
void WindowParser<S>::Begin(const Mft::SlotWindow& window) {
  End();
  window_ = window;
  const auto slots = static_cast<size_t>(window.last - window.first);
  if (outcomes_.size() < slots) {
    outcomes_.resize(slots);
  }
  const size_t chunks = (slots + chunk_slots_ - 1) / chunk_slots_;
  // The logs of the window before stay as they are: the lines its last slot
  // logged when its attributes were dropped are replayed with the next slot.
  log_set_ = 1 - log_set_;
  std::vector<Log::Detail::CapturedLog>& logs = logs_.at(log_set_);
  if (logs.size() < chunks) {
    logs.resize(chunks);
  }
  // Asked here, on the thread that owns the sinks: a worker never does.
  levels_ = Log::Detail::EnabledLevels();
  waited_chunk_ = no_chunk;
  active_ = true;
  pool_.Start(chunks,
              [this](size_t chunk, size_t slot) { ParseChunk(chunk, slot); });
}

template <Cache::Strategy S>
void WindowParser<S>::End() {
  if (active_) {
    active_ = false;
    pool_.Finish();
  }
}

template <Cache::Strategy S>
bool WindowParser<S>::Covers(ULONGLONG record) const noexcept {
  return active_ && record >= window_.first && record < window_.last;
}

template <Cache::Strategy S>
SlotOutcome& WindowParser<S>::Take(ULONGLONG record) {
  const auto index = static_cast<size_t>(record - window_.first);
  const size_t chunk = index / chunk_slots_;
  if (chunk != waited_chunk_) {
    const Bench::Detail::SliceScope wait(Bench::Slice::PoolWait);
    pool_.WaitFor(chunk);
    waited_chunk_ = chunk;
  }
  return outcomes_.at(index);
}

template <Cache::Strategy S>
const Log::Detail::CapturedLog& WindowParser<S>::LogOf(ULONGLONG record) const {
  const auto index = static_cast<size_t>(record - window_.first);
  return logs_.at(log_set_).at(index / chunk_slots_);
}

template <Cache::Strategy S>
void WindowParser<S>::ParseChunk(size_t chunk, size_t slot) {
  Log::Detail::CapturedLog& log = logs_.at(log_set_).at(chunk);
  log.Reset(levels_);
  const Log::Detail::CaptureScope capture(log);
  Io::FileRecord<S>& file_record = *contexts_.at(slot);

  const auto slots = static_cast<size_t>(window_.last - window_.first);
  const size_t first = chunk * chunk_slots_;
  const size_t last = (std::min)(first + chunk_slots_, slots);
  for (size_t index = first; index < last; index++) {
    const ULONGLONG record = window_.first + index;
    if (bitmap_ != nullptr && IsMarkedFree(*bitmap_, record)) {
      continue;
    }
    SlotOutcome& outcome = outcomes_.at(index);
    outcome = SlotOutcome{};
    outcome.log_begin = log.End();
    ParseWindowSlot(
        file_record, *volume_,
        {.record = record,
         .bytes = window_.bytes.subspan(index * record_size_, record_size_)},
        features_, outcome);
    outcome.log_end = log.End();
    // What the attributes log when they are dropped is kept apart: it
    // belongs after what the scan does with the slot.
    file_record.ClearAttrs();
    outcome.release_end = log.End();
  }
}

template <Cache::Strategy S>
WaitingParser<S>::Context::Context(const NtfsVolume<S>& volume)
    : record(volume) {}

template <Cache::Strategy S>
WaitingParser<S>::WaitingParser(const NtfsVolume<S>& volume, size_t threads,
                                const HeldRecords& held, const SlotBases& bases,
                                SlotFeatures features)
    : volume_(&volume),
      held_(&held),
      waiting_(&held.WaitingRecords()),
      chunk_records_(
          (std::max<size_t>)(1, volume.GetOptions()
                                    .tuning.mft.waiting_chunk_records)),
      features_(features),
      pool_(threads - 1) {
  contexts_.reserve(pool_.Slots());
  for (size_t slot = 0; slot < pool_.Slots(); slot++) {
    Context& context =
        *contexts_.emplace_back(std::make_unique<Context>(volume));
    context.record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
    if (features.account) {
      context.record.SetClaimMask(Attr::Mask::All);
    }
    context.record.SetExtensionSource(
        [&held](ULONGLONG record) { return held.FindExtension(record); });
    context.record.SetBaseSource(
        [&bases](ULONGLONG record) { return bases.BaseOf(record); });
    context.record.SetReadRefusal(&context.refused);
  }
}

template <Cache::Strategy S>
void WaitingParser<S>::Begin(size_t first, size_t count) {
  End();
  first_ = first;
  count_ = count;
  if (outcomes_.size() < count) {
    outcomes_.resize(count);
  }
  const size_t chunks = (count + chunk_records_ - 1) / chunk_records_;
  // The logs of the batch before stay as they are: the lines its last record
  // logged when its attributes were dropped are replayed with the next one.
  log_set_ = 1 - log_set_;
  std::vector<Log::Detail::CapturedLog>& logs = logs_.at(log_set_);
  if (logs.size() < chunks) {
    logs.resize(chunks);
  }
  // Asked here, on the thread that owns the sinks: a worker never does.
  levels_ = Log::Detail::EnabledLevels();
  waited_chunk_ = no_chunk;
  active_ = true;
  pool_.Start(chunks,
              [this](size_t chunk, size_t slot) { ParseChunk(chunk, slot); });
}

template <Cache::Strategy S>
void WaitingParser<S>::End() {
  if (active_) {
    active_ = false;
    pool_.Finish();
  }
}

template <Cache::Strategy S>
bool WaitingParser<S>::Covers(size_t index) const noexcept {
  return active_ && index >= first_ && index < first_ + count_;
}

template <Cache::Strategy S>
SlotOutcome& WaitingParser<S>::Take(size_t index) {
  const size_t chunk = (index - first_) / chunk_records_;
  if (chunk != waited_chunk_) {
    const Bench::Detail::SliceScope wait(Bench::Slice::PoolWait);
    pool_.WaitFor(chunk);
    waited_chunk_ = chunk;
  }
  return outcomes_.at(index - first_);
}

template <Cache::Strategy S>
const Log::Detail::CapturedLog& WaitingParser<S>::LogOf(size_t index) const {
  return logs_.at(log_set_).at((index - first_) / chunk_records_);
}

template <Cache::Strategy S>
void WaitingParser<S>::ParseChunk(size_t chunk, size_t slot) {
  Log::Detail::CapturedLog& log = logs_.at(log_set_).at(chunk);
  log.Reset(levels_);
  const Log::Detail::CaptureScope capture(log);
  Context& context = *contexts_.at(slot);

  const size_t first = chunk * chunk_records_;
  const size_t last = (std::min)(first + chunk_records_, count_);
  for (size_t offset = first; offset < last; offset++) {
    SlotOutcome& outcome = outcomes_.at(offset);
    outcome = SlotOutcome{};
    const HeldRecords::Waiting& waiting = waiting_->at(first_ + offset);
    // Its reads are the scan's thread's, and so is its place in the log.
    if (!waiting.slot || !waiting.list_extents.empty() ||
        !waiting.unheld_extensions.empty()) {
      outcome.needs_scan_thread = true;
      continue;
    }
    context.refused = false;
    outcome.log_begin = log.End();
    ParseWaiting(context.record, waiting, outcome);
    outcome.log_end = log.End();
    // What the attributes log when they are dropped is kept apart: it
    // belongs after what the scan does with the record.
    context.record.ClearAttrs();
    outcome.release_end = log.End();
    if (context.refused) {
      outcome = SlotOutcome{};
      outcome.needs_scan_thread = true;
    }
  }
}

template <Cache::Strategy S>
void WaitingParser<S>::ParseWaiting(Io::FileRecord<S>& file_record,
                                    const HeldRecords::Waiting& waiting,
                                    SlotOutcome& outcome) const {
  outcome.kind = SlotKind::Base;
  const bool parsed =
      file_record.ParseFileRecord(waiting.record, held_->Bytes(*waiting.slot));
  const BaseSlot base{.record = waiting.record,
                      .header_damaged = waiting.header_damaged};
  outcome.kept = parsed && ReadBaseEntry(file_record, *volume_, base,
                                         outcome.counts, outcome.entry);
  if (outcome.kept) {
    NoteKept(file_record, base, features_, outcome);
  }
}

template class WindowParser<Cache::Strategy::NoCache>;
template class WindowParser<Cache::Strategy::FullCache>;
template class WaitingParser<Cache::Strategy::NoCache>;
template class WaitingParser<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
