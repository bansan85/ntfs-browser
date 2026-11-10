#include "mft/slot-scan.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "alloc/claim-inventory.h"
#include "alloc/types.h"
#include "attr/mask.h"
#include "attr/type.h"
#include "bench/bench.h"
#include "bench/recorder.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/data-state-resolver.h"
#include "mft/held-records.h"
#include "mft/slot-entry.h"
#include "mft/slot-parsers.h"
#include "mft/slot-reader.h"
#include "mft/tree-testing.h"
#include "mft/tree.h"
#include "mft/used-slots.h"
#include "mft/waiting-reads.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Records between two progress() calls: often enough for a live progress
// line, rare enough to cost nothing next to a record read.
constexpr ULONGLONG progress_interval = 4096;

// The cap a test asked for. nullopt means the default.
std::optional<size_t> max_held_record_bytes_override;

// Slots the scans of this thread took from the threads that parsed them. Only
// the thread that owns a scan changes it, and tests are the only readers.
thread_local size_t slots_parsed_by_threads = 0;

// The limits a test asked for, and the verdict it imposed on the probe. nullopt
// means the default, and the measure.
std::optional<ULONGLONG> min_auto_threaded_slots_override;
std::optional<ULONGLONG> probe_slots_override;
std::optional<bool> probe_verdict_override;

// The batch length a test asked for. nullopt means the default.
std::optional<size_t> waiting_batch_records_override;

// The limit a test asked for. nullopt means the default.
std::optional<size_t> min_auto_threaded_waiting_override;

// Waiting records the scans of this thread took from the threads that parsed
// them, in the second pass. Read by tests only, like slots_parsed_by_threads.
thread_local size_t waiting_parsed_by_threads = 0;

// Threads a caller can ask for at most. A forged count cannot start more.
constexpr unsigned max_scan_threads = 64;

// The values Tuning::MftTuning has when nothing sets them: the one source of
// every default. The caps a test asked for sit on top of them.
const Tuning::MftTuning& DefaultMft() {
  static const Tuning::MftTuning defaults;
  return defaults;
}

// Whether the scan goes on at record. Every progress_interval records, the
// caller follows along through options.progress and may stop the scan.
bool ProgressAllowsRecord(const Mft::MftTree::ScanOptions& options,
                          ULONGLONG record, ULONGLONG total) {
  return !options.progress || record % progress_interval != 0 ||
         options.progress(record, total);
}

// The bytes of raw records a scan of volume holds for its second pass at most:
// the cap a test asked for, else the tuning, kept inside what size_t can count.
template <Cache::Strategy S>
size_t HeldBudgetBytes(const NtfsVolume<S>& volume) {
  const std::uint64_t budget =
      max_held_record_bytes_override
          ? *max_held_record_bytes_override
          : volume.GetOptions().tuning.mft.max_held_record_bytes;
  return static_cast<size_t>(
      (std::min<std::uint64_t>)(budget, (std::numeric_limits<size_t>::max)()));
}

// Removes the entries at indices, sorted, and keeps the others in order.
void DropEntries(std::vector<Mft::MftTree::Entry>& entries,
                 const std::vector<size_t>& indices) {
  if (indices.empty()) {
    return;
  }
  size_t kept = 0;
  size_t next_drop = 0;
  for (size_t index = 0; index < entries.size(); index++) {
    if (next_drop < indices.size() && indices.at(next_drop) == index) {
      next_drop++;
      continue;
    }
    if (kept != index) {
      entries.at(kept) = std::move(entries.at(index));
    }
    kept++;
  }
  entries.erase(std::next(entries.begin(), gsl::narrow<std::ptrdiff_t>(kept)),
                entries.end());
}

// The threads the scan parses with. options.threads gives it, except for 0: a
// volume with enough slots takes the threads of the machine, a small one takes
// one. A volume with a raw attribute callback takes one whatever is asked.
template <Cache::Strategy S>
size_t ScanThreads(const NtfsVolume<S>& volume,
                   const Mft::MftTree::ScanOptions& options, ULONGLONG total) {
  if (volume.HasAttrRawCB()) {
    return 1;
  }
  unsigned threads = options.threads;
  if (threads == 0) {
    const Tuning::MftTuning& tuning = volume.GetOptions().tuning.mft;
    if (total < min_auto_threaded_slots_override.value_or(
                    tuning.min_auto_threaded_slots)) {
      return 1;
    }
    threads = (std::min)(std::thread::hardware_concurrency(),
                         tuning.max_auto_threads);
  }
  return (std::clamp)(threads, 1U, max_scan_threads);
}

// The two passes over the record slots of $MFT that make the entries of a
// scan, in record order, through one reused FileRecord. The first pass reads
// each slot, and keeps the entry of each base record. A base record with an
// $ATTRIBUTE_LIST waits for the second pass: by then the first one has held the
// extension records it asks for, so they come from memory instead of one disk
// read each. What does not fit in memory is read in the second pass, announced
// to the reader ahead of it.
//
// With more than one thread, the first pass has several threads parse the slots
// of each window, and takes their outcomes in record order. Only the thread
// that owns the scan reads the volume, logs, and changes the scan: a slot that
// needs a read is parsed again by it, as it is with one thread.
template <Cache::Strategy S>
class SlotScan {
 public:
  // entries and stats belong to the caller, and receive the result.
  SlotScan(const NtfsVolume<S>& volume, const SlotScanInput& input,
           std::vector<Mft::MftTree::Entry>& entries,
           Mft::MftTree::ScanStats& stats)
      : volume_(&volume),
        options_(&input.options),
        bitmap_(&input.bitmap),
        features_(input.features),
        entries_(&entries),
        stats_(&stats),
        total_(volume.GetRecordsCount()),
        held_(volume.GetFileRecordSize(),
              volume.GetOptions().tuning.mft.held_block_bytes,
              HeldBudgetBytes(volume)),
        bases_(total_),
        unheld_(volume),
        file_record_(volume),
        resolver_(volume, entries, stats, input.features.account),
        slots_(volume, volume.GetOptions().tuning.mft.window_bytes) {
    ConfigureFileRecord();
    if (input.bitmap) {
      slots_.Plan(PlanSlotRanges(*input.bitmap, total_,
                                 volume.GetOptions().tuning.mft.max_gap_slots));
    }
    parse_threads_ = ScanThreads(volume, input.options, total_);
    // An explicit count is used from the first window. A count left to the
    // scan waits for the probe.
    automatic_ = input.options.threads == 0;
  }

  SlotScan(const SlotScan&) = delete;
  SlotScan& operator=(const SlotScan&) = delete;
  SlotScan(SlotScan&&) = delete;
  SlotScan& operator=(SlotScan&&) = delete;

  // The attributes of the last slot a thread parsed are dropped here, as the
  // scan of one thread drops its own.
  ~SlotScan() {
    try {
      DropLastParsedByThread();
    }
    // NOLINTNEXTLINE(bugprone-empty-catch)
    catch (...) {
      // A destructor cannot report a failure, and nothing is left to undo.
    }
  }

  // Reads the slots in record order, and keeps the entries.
  void FirstPass() {
    for (ULONGLONG record = 0; record < total_; record++) {
      if (!ProgressAllowsRecord(*options_, record, total_)) {
        stats_->complete = false;
        break;
      }

      held_.ReleaseBefore(record);
      if (*bitmap_ && IsMarkedFree(**bitmap_, record)) {
        stats_->skipped++;
        continue;
      }
      if (!TakeParsed(record)) {
        ParseHere(record);
      }
    }

    // The threads rest during the second pass, which they have no part in.
    if (parser_) {
      parser_->End();
    }
    held_.Finish();
  }

  // The clusters of $J, when the scan met the $UsnJrnl record: empty otherwise.
  [[nodiscard]] std::vector<Alloc::Extent> TakeJournalExtents() {
    return std::move(journal_extents_);
  }

  // The claims of every record the scan kept, empty when it did not account.
  [[nodiscard]] Alloc::ClaimInventory TakeInventory() {
    return std::move(inventory_);
  }

  // Parses the base records that wait for their extension records, and drops
  // the entries the scan does not keep.
  void SecondPass() {
    std::vector<size_t> dropped;
    const std::vector<HeldRecords::Waiting>& waiting_records =
        held_.WaitingRecords();
    ReadAnnouncer<S> announcer(*volume_, waiting_records);
    const bool on_threads = WaitingParsedByThreads(waiting_records.size());
    for (size_t index = 0; index < waiting_records.size(); index++) {
      if (announcer.RestartDue()) {
        announcer.Restart(index);
      }
      announcer.Before(index);
      ReplayPendingRelease();
      const std::optional<bool> taken =
          on_threads ? TakeWaiting(index) : std::nullopt;
      const bool kept =
          taken ? *taken : ParseWaitingHere(waiting_records.at(index));
      if (!kept) {
        dropped.push_back(waiting_records.at(index).entry_index);
      }
    }
    EndSecondPass(dropped);
  }

 private:
  // Has file_record_ find the extension records in the held ones first, then in
  // the ones the second pass read, and the base of a slot in the first pass's
  // notes.
  void ConfigureFileRecord() {
    file_record_.SetParseOptions(
        {.attr_mask = Attr::Mask::FileName | Attr::Mask::Data,
         .claim_mask =
             features_.account ? Attr::Mask::All : static_cast<Attr::Mask>(0),
         .extension_source =
             [this](ULONGLONG record) {
               const std::span<const BYTE> bytes = held_.FindExtension(record);
               return bytes.empty() ? unheld_.Find(record) : bytes;
             },
         .base_source =
             [this](ULONGLONG record) { return bases_.BaseOf(record); }});
  }

  // Ends the second pass: the entries at dropped are not kept.
  void EndSecondPass(const std::vector<size_t>& dropped) {
    // The threads rest while the entries are resolved and dropped.
    if (waiting_parser_) {
      waiting_parser_->End();
    }
    if (features_.annotate) {
      resolver_.Finish();
    }
    DropEntries(*entries_, dropped);
  }

  // Adds the counters of one slot to the statistics, and keeps where $J lies.
  void Merge(const RecordCounts& counts) {
    AddCounts(*stats_, counts);
    if (!counts.journal_extents.empty()) {
      journal_extents_ = counts.journal_extents;
    }
  }

  // Gives the resolver and the inventory what the record in file_record_, which
  // was just kept as entry entry_index, says of its data and its clusters.
  void NoteKept(size_t entry_index, const BaseSlot& slot,
                const RecordCounts& counts) {
    if (features_.annotate) {
      resolver_.Add(entry_index, ComputeDataNote(file_record_));
    }
    if (features_.account) {
      inventory_.Add(ComputeClaims(file_record_, slot, counts));
    }
  }

  // Gives the resolver and the inventory what a thread found in outcome, whose
  // entry was just kept as entry entry_index.
  void Adopt(size_t entry_index, SlotOutcome& outcome) {
    if (features_.annotate) {
      resolver_.Add(entry_index, std::move(outcome.note));
    }
    if (features_.account) {
      inventory_.Add(std::move(outcome.claims));
    }
  }

  // Whether the threads parse the count waiting records of the second pass. An
  // explicit thread count always has them. A count left to the scan has them
  // from min_auto_threaded_waiting records on, whatever the first pass found:
  // the second pass reads next to nothing, so it is bound by the CPU even
  // where the first one was bound by the volume.
  [[nodiscard]] bool WaitingParsedByThreads(size_t count) const {
    if (parse_threads_ <= 1) {
      return false;
    }
    return !automatic_ ||
           count >=
               min_auto_threaded_waiting_override.value_or(
                   volume_->GetOptions().tuning.mft.min_auto_threaded_waiting);
  }

  // Parses the header of waiting on this thread: from the bytes the first pass
  // parsed, so that it parses as it did then, or, for a record that was not
  // held, read again and announced to the reader.
  bool ParseWaitingRecord(const HeldRecords::Waiting& waiting) {
    return waiting.slot ? file_record_.ParseFileRecord(
                              waiting.record, held_.Bytes(*waiting.slot))
                        : file_record_.ParseFileRecord(waiting.record);
  }

  // Parses waiting on this thread, with the reads that takes, and keeps what it
  // gives. True when its entry is kept.
  bool ParseWaitingHere(const HeldRecords::Waiting& waiting) {
    const Bench::Detail::SliceScope slice(Bench::Slice::Parse);
    Mft::MftTree::Entry& entry = entries_->at(waiting.entry_index);
    const bool parsed = ParseWaitingRecord(waiting);
    parsed_here_ = true;
    // Read even when the record is dropped: the reader was told to.
    unheld_.Load(waiting.unheld_extensions);
    RecordCounts counts;
    const BaseSlot slot{.record = waiting.record,
                        .header_damaged = waiting.header_damaged};
    const bool kept = parsed && ReadBaseEntry(Context(), slot, counts, entry);
    Merge(counts);
    unheld_.Clear();
    if (kept) {
      NoteKept(waiting.entry_index, slot, counts);
    }
    return kept;
  }

  // The outcome the threads gave for waiting record index, giving them the
  // batch when the scan reaches it.
  SlotOutcome& WaitingOutcome(size_t index) {
    if (!waiting_parser_) {
      waiting_parser_.emplace(*volume_, parse_threads_,
                              WaitingSources{held_, bases_}, features_);
    }
    if (!waiting_parser_->Covers(index)) {
      const size_t batch =
          (std::max)(size_t{1}, waiting_batch_records_override.value_or(
                                    volume_->GetOptions()
                                        .tuning.mft.waiting_batch_records));
      waiting_parser_->Begin(
          index, (std::min)(batch, held_.WaitingRecords().size() - index));
    }
    return waiting_parser_->Take(index);
  }

  // Takes waiting record index from the batch the threads parse, and keeps what
  // they found. Nothing when the scan's thread has to parse the record: it
  // reads the volume. Else whether its entry is kept.
  std::optional<bool> TakeWaiting(size_t index) {
    const Bench::Detail::SliceScope slice(Bench::Slice::Merge);
    SlotOutcome& outcome = WaitingOutcome(index);
    if (outcome.needs_scan_thread) {
      return std::nullopt;
    }
    waiting_parsed_by_threads++;

    const HeldRecords::Waiting& waiting = held_.WaitingRecords().at(index);
    // The record before is gone first, so that what its attributes log comes
    // before the lines of this one.
    if (parsed_here_) {
      file_record_.ClearAttrs();
      parsed_here_ = false;
    }
    const Log::Detail::CapturedLog& log = waiting_parser_->LogOf(index);
    log.Replay(outcome.log_begin, outcome.log_end);
    Merge(outcome.counts);
    if (outcome.kept) {
      entries_->at(waiting.entry_index) = std::move(outcome.entry);
      Adopt(waiting.entry_index, outcome);
    }
    LeaveRelease(log, outcome, waiting.record, held_.Bytes(*waiting.slot));
    return outcome.kept;
  }

  // Takes slot record from the window the threads parse, giving them the window
  // when the scan reaches it. False when the thread that owns the scan has to
  // parse the slot: no window serves it, or it needs the volume.
  bool TakeParsed(ULONGLONG record) {
    if (parse_threads_ <= 1 || record < serial_until_) {
      return false;
    }
    const Bench::Detail::SliceScope slice(Bench::Slice::Merge);
    SlotOutcome* const outcome = OutcomeOf(record);
    if (outcome == nullptr || outcome->needs_scan_thread) {
      return false;
    }
    slots_parsed_by_threads++;
    if (outcome->kind == SlotKind::Base && outcome->has_list) {
      return TakeListed(record, *outcome);
    }
    TakeKept(record, *outcome);
    return true;
  }

  // The outcome the threads gave for slot record, loading and giving them its
  // window when the scan reaches it. Null when this thread parses the slot.
  SlotOutcome* OutcomeOf(ULONGLONG record) {
    if ((!parser_ || !parser_->Covers(record)) && !BeginWindow(record)) {
      return nullptr;
    }
    return &parser_->Take(record);
  }

  // Loads the window that holds record, and has the threads parse it. False
  // when no window serves the slot, or this thread parses the window itself.
  bool BeginWindow(ULONGLONG record) {
    if (parser_) {
      // The next window overwrites the bytes the threads parse.
      parser_->End();
    }
    const auto began = std::chrono::steady_clock::now();
    const std::optional<Mft::SlotWindow> window = slots_.ServeWindow(record);
    const auto loaded = std::chrono::steady_clock::now();
    if (!window) {
      return false;
    }
    if (!ThreadsParse(record, began, loaded)) {
      // This thread parses the window it just loaded.
      serial_until_ = window->last;
      return false;
    }
    if (!parser_) {
      parser_.emplace(*volume_, parse_threads_, *bitmap_ ? &**bitmap_ : nullptr,
                      features_);
    }
    parser_->Begin(*window);
    return true;
  }

  // Keeps the base record record, which has an $ATTRIBUTE_LIST, as a record
  // that waits for the second pass. False when its bytes are not a record.
  bool TakeListed(ULONGLONG record, SlotOutcome& outcome) {
    // The window holds the slot in full, so the record waits.
    if (!held_.HoldWaiting({.record = record,
                            .entry_index = entries_->size(),
                            .header_damaged = outcome.header_damaged,
                            .list_extents = std::move(outcome.list_extents)},
                           slots_.HeldSlot(record))) {
      return false;
    }
    Merge(record, outcome);
    entries_->emplace_back();
    LeaveRelease(record, outcome);
    return true;
  }

  // Keeps what the threads found in slot record, which has no list.
  void TakeKept(ULONGLONG record, SlotOutcome& outcome) {
    Merge(record, outcome);
    if (outcome.kind == SlotKind::Extension) {
      // Its base record may come later, and then takes it from memory.
      held_.MeetExtension(record, outcome.base_reference,
                          slots_.HeldSlot(record));
    }
    if (outcome.kind == SlotKind::Base && outcome.kept) {
      entries_->push_back(std::move(outcome.entry));
      Adopt(entries_->size() - 1, outcome);
    }
    LeaveRelease(record, outcome);
  }

  // Whether the threads parse the window just loaded for slot record, which
  // took from began to loaded to load. With an explicit thread count, they
  // always do. With a count left to the scan, they do once a probe says they
  // pay: the first probe_slots slots are parsed by this thread alone, and the
  // time the windows took to load, against the time the probe lasted, tells
  // whether the volume is read faster than this thread parses. The choice is
  // made once.
  bool ThreadsParse(ULONGLONG record,
                    std::chrono::steady_clock::time_point began,
                    std::chrono::steady_clock::time_point loaded) {
    if (!automatic_) {
      return true;
    }
    if (probe_.decided) {
      return probe_.threads_pay;
    }
    if (!probe_.started) {
      probe_.started = true;
      probe_.first_slot = record;
      probe_.began = began;
    }
    probe_.load_seconds +=
        std::chrono::duration<double>(loaded - began).count();
    const Tuning::MftTuning& tuning = volume_->GetOptions().tuning.mft;
    if (record - probe_.first_slot <
        probe_slots_override.value_or(tuning.probe_slots)) {
      return false;
    }
    probe_.decided = true;
    probe_.threads_pay = probe_verdict_override.value_or(ScanThreadsPay(
        probe_.load_seconds,
        std::chrono::duration<double>(loaded - probe_.began).count(),
        static_cast<double>(tuning.max_load_per_mille) / 1000.0));
    return probe_.threads_pay;
  }

  // Makes the scan take what the threads found in slot record: its log lines,
  // its counters, and the base it names. The slot before it is gone first, so
  // that what its attributes log comes before the lines of this one.
  void Merge(ULONGLONG record, const SlotOutcome& outcome) {
    ReplayPendingRelease();
    if (parsed_here_) {
      file_record_.ClearAttrs();
      parsed_here_ = false;
    }
    parser_->LogOf(record).Replay(outcome.log_begin, outcome.log_end);
    Merge(outcome.counts);
    if (outcome.kind != SlotKind::Unreadable) {
      bases_.Note(record, outcome.base_reference);
    }
  }

  // Keeps the lines the attributes of slot record logged when a thread dropped
  // them, for the next slot to be parsed: one thread drops the attributes of a
  // slot when it parses the next one.
  void LeaveRelease(ULONGLONG record, const SlotOutcome& outcome) {
    LeaveRelease(parser_->LogOf(record), outcome, record, {});
  }

  // Same for a record in log, whose bytes are held. A record of the second
  // pass gives them, since no window holds it: held is empty for a slot of the
  // first pass.
  void LeaveRelease(const Log::Detail::CapturedLog& log,
                    const SlotOutcome& outcome, ULONGLONG record,
                    std::span<const BYTE> held) {
    if (outcome.release_end.lines > outcome.log_end.lines) {
      pending_release_ = {.log = &log,
                          .begin = outcome.log_end,
                          .end = outcome.release_end,
                          .slot = record,
                          .held = held};
    }
  }

  // Ends the scan for the attributes LeaveRelease() kept. One thread drops
  // the attributes of its last slot when its FileRecord goes away, which
  // does it member by member in the reverse of the order ClearAttrs() uses. To
  // log them in that order, the slot is parsed again, without a sound, in
  // file_record_, whose destructor then does it.
  void DropLastParsedByThread() {
    if (pending_release_.log == nullptr || !pending_release_.slot) {
      ReplayPendingRelease();
      return;
    }
    const std::span<const BYTE> bytes =
        pending_release_.held.empty() ? slots_.HeldSlot(*pending_release_.slot)
                                      : pending_release_.held;
    pending_release_.log = nullptr;
    if (bytes.empty()) {
      return;
    }
    Log::Detail::CapturedLog silent;
    silent.Reset(0);
    const Log::Detail::CaptureScope capture(silent);
    if (file_record_.ParseFileRecord(*pending_release_.slot, bytes)) {
      static_cast<void>(TryParseAttrs(file_record_));
    }
  }

  // Sends the lines LeaveRelease() kept to the sinks.
  void ReplayPendingRelease() {
    if (pending_release_.log != nullptr) {
      pending_release_.log->Replay(pending_release_.begin,
                                   pending_release_.end);
      pending_release_.log = nullptr;
    }
  }

  // Parses slot record on this thread, and keeps what it gives.
  void ParseHere(ULONGLONG record) {
    const Bench::Detail::SliceScope slice(Bench::Slice::Parse);
    ReplayPendingRelease();
    RecordCounts counts;
    ParseHereInto(record, counts);
    Merge(counts);
  }

  // Holds the base record record, which file_record_ holds, for the second pass
  // when it has an $ATTRIBUTE_LIST. Its entry keeps its place, so the entries
  // stay in record order. True when it waits. A record that has a list and
  // cannot wait is parsed at once, and takes the extension records held for it.
  bool HoldListed(ULONGLONG record, bool header_damaged) {
    if (!file_record_.HasAttrHeader(Attr::Type::AttributeList)) {
      return false;
    }
    if (held_.HoldWaiting({.record = record,
                           .entry_index = entries_->size(),
                           .header_damaged = header_damaged,
                           .list_extents = file_record_.AttributeListExtents(
                               max_announced_list_bytes)},
                          slots_.HeldSlot(record))) {
      entries_->emplace_back();
      return true;
    }
    held_.Confirm(record);
    return false;
  }

  // The record file_record_ is parsed in, with its volume.
  SlotContext<S> Context() { return {file_record_, *volume_}; }

  // The body of ParseHere(): counts receives what the slot adds to the
  // statistics.
  void ParseHereInto(ULONGLONG record, RecordCounts& counts) {
    bool header_damaged = false;
    const bool parsed = slots_.ParseFileRecord(file_record_, record);
    const SlotKind kind =
        ClassifySlot(Context(), parsed, counts, header_damaged);
    if (kind == SlotKind::Unreadable) {
      return;
    }
    bases_.Note(record, file_record_.GetBaseRecordReference());
    if (kind == SlotKind::Extension) {
      // Its base record may come later, and then takes it from memory.
      held_.MeetExtension(record, file_record_.GetBaseRecordReference(),
                          slots_.HeldSlot(record));
      return;
    }
    if (kind == SlotKind::Dropped) {
      return;
    }
    if (HoldListed(record, header_damaged)) {
      return;
    }

    // Filled in place: an Entry is big, and most slots are kept.
    Mft::MftTree::Entry& entry = entries_->emplace_back();
    parsed_here_ = true;
    const BaseSlot slot{.record = record, .header_damaged = header_damaged};
    if (!ReadBaseEntry(Context(), slot, counts, entry)) {
      entries_->pop_back();
      return;
    }
    NoteKept(entries_->size() - 1, slot, counts);
  }

  const NtfsVolume<S>* volume_;
  const Mft::MftTree::ScanOptions* options_;
  const std::optional<std::vector<BYTE>>* bitmap_;
  SlotFeatures features_;
  std::vector<Mft::MftTree::Entry>* entries_;
  Mft::MftTree::ScanStats* stats_;
  ULONGLONG total_;
  HeldRecords held_;
  SlotBases bases_;
  UnheldExtensions<S> unheld_;
  std::vector<Alloc::Extent> journal_extents_;
  // The claims of the kept records, when the scan accounts for clusters.
  Alloc::ClaimInventory inventory_;
  Io::FileRecord<S> file_record_;
  DataStateResolver<S> resolver_;
  // Reads $MFT in big blocks, not record by record, and only the used ones.
  Mft::SlotReader<S> slots_;
  // Threads that parse, this one included, and whether the scan chooses when
  // they start. With one, this thread parses every slot.
  size_t parse_threads_{1};
  bool automatic_{false};

  // What the probe of an automatic scan has seen so far.
  struct Probe {
    bool started{false};
    bool decided{false};
    bool threads_pay{false};
    ULONGLONG first_slot{0};
    std::chrono::steady_clock::time_point began;
    double load_seconds{0};
  };

  Probe probe_;
  // The slots before this one are in a window this thread parses itself.
  ULONGLONG serial_until_{0};
  // Whether file_record_ holds the attributes of a slot this thread parsed.
  bool parsed_here_{false};

  // The lines the attributes of the last slot a thread parsed logged when they
  // were dropped, which no slot has taken its turn to replay yet.
  struct PendingRelease {
    const Log::Detail::CapturedLog* log{nullptr};
    Log::Detail::CapturedLog::Mark begin;
    Log::Detail::CapturedLog::Mark end;
    // The slot those attributes belong to, and its bytes when no window of
    // slots holds them: a waiting record of the second pass.
    std::optional<ULONGLONG> slot;
    std::span<const BYTE> held;
  };

  PendingRelease pending_release_;
  // Last: its threads read the window of slots_, so they end first.
  std::optional<WindowParser<S>> parser_;
  // Starts with the second pass. Its threads read held_ and bases_, and its
  // logs may be the ones pending_release_ points at.
  std::optional<WaitingParser<S>> waiting_parser_;
};

}  // namespace

void SetAutoThreadLimitsForTests(std::optional<ULONGLONG> min_slots,
                                 std::optional<ULONGLONG> probe_slots,
                                 std::optional<bool> verdict,
                                 std::optional<size_t> min_waiting) {
  min_auto_threaded_slots_override = min_slots;
  probe_slots_override = probe_slots;
  probe_verdict_override = verdict;
  min_auto_threaded_waiting_override = min_waiting;
}

bool ScanThreadsPay(double load_seconds, double elapsed_seconds,
                    double max_load_share) noexcept {
  return elapsed_seconds > 0 &&
         load_seconds <= max_load_share * elapsed_seconds;
}

bool ScanThreadsPay(double load_seconds, double elapsed_seconds) noexcept {
  return ScanThreadsPay(load_seconds, elapsed_seconds,
                        static_cast<double>(DefaultMft().max_load_per_mille) /
                            1000.0);
}

size_t MaxHeldRecordBytes() {
  return max_held_record_bytes_override.value_or(static_cast<size_t>(
      (std::min<std::uint64_t>)(DefaultMft().max_held_record_bytes,
                                (std::numeric_limits<size_t>::max)())));
}

void SetMaxHeldRecordBytesForTests(std::optional<size_t> limit) {
  max_held_record_bytes_override = limit;
}

void SetWaitingBatchRecordsForTests(std::optional<size_t> records) {
  waiting_batch_records_override = records;
}

size_t WaitingParsedByThreadsForTests(bool reset) noexcept {
  const size_t count = waiting_parsed_by_threads;
  if (reset) {
    waiting_parsed_by_threads = 0;
  }
  return count;
}

size_t SlotsParsedByThreadsForTests(bool reset) noexcept {
  const size_t count = slots_parsed_by_threads;
  if (reset) {
    slots_parsed_by_threads = 0;
  }
  return count;
}

template <Cache::Strategy S>
std::vector<Alloc::Extent> ScanSlots(const NtfsVolume<S>& volume,
                                     const SlotScanInput& input,
                                     const SlotScanOutput& output) {
  SlotScan<S> scan(volume, input, output.entries, output.stats);
  {
    const Bench::PhaseScope phase(Bench::Phase::Pass1);
    volume.SetReadPattern(Tuning::Pattern::LargeSequential);
    scan.FirstPass();
  }
  if (!Bench::StopRequested()) {
    const Bench::PhaseScope phase(Bench::Phase::Pass2);
    volume.SetReadPattern(Tuning::Pattern::ScatteredGrouped);
    scan.SecondPass();
  }
  output.inventory = scan.TakeInventory();
  return scan.TakeJournalExtents();
}

template std::vector<Alloc::Extent> ScanSlots<Cache::Strategy::NoCache>(
    const NtfsVolume<Cache::Strategy::NoCache>&, const SlotScanInput&,
    const SlotScanOutput&);
template std::vector<Alloc::Extent> ScanSlots<Cache::Strategy::FullCache>(
    const NtfsVolume<Cache::Strategy::FullCache>&, const SlotScanInput&,
    const SlotScanOutput&);

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
