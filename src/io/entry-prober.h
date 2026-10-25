#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

template <Cache::Strategy S>
class NtfsVolume;

namespace Io {

// What the probe of the record an index entry names found: the header, and
// nothing of the attributes.
struct NamedState {
  // False when the record does not parse.
  bool parsed{false};
  // True when the record is freed. Meaningful only when parsed.
  bool deleted{false};
  // How many times the record was reused. Meaningful only when parsed.
  WORD sequence{0};
};

// Probes the records that the entries of a directory name, a group of entries
// at a time, for DirectoryIndex's batched walk. A group is probed in $MFT
// order, neighbouring records in one read, the reads announced to the volume's
// reader first. A record is probed once per prober whatever number of entries
// name it, as far as the memo goes. What a probe logged is held with it, and
// replayed each time an entry takes the outcome: the log is the one of probing
// at the entry. Short-lived, like the walk that owns it. Not thread safe.
template <Cache::Strategy S>
class EntryProber {
 public:
  // volume MUST outlive the prober.
  explicit EntryProber(const NtfsVolume<S>& volume);
  EntryProber(const EntryProber&) = delete;
  EntryProber& operator=(const EntryProber&) = delete;
  EntryProber(EntryProber&&) = delete;
  EntryProber& operator=(EntryProber&&) = delete;
  ~EntryProber() = default;

  // Probes the records, the ones the memo does not hold, in one batch. They
  // may come in any order, and repeat.
  void ProbeGroup(std::span<const ULONGLONG> records);

  // The state of record, probed now when the memo does not hold it. Replays
  // the lines its probe logged, and counts the conditions it salvaged, as the
  // probe at this very place would.
  [[nodiscard]] NamedState Take(ULONGLONG record);

 private:
  // The memo entry of a probed record.
  struct Known {
    NamedState state;
    // True when logs_ holds lines of the probe.
    bool logged{false};
  };

  // What one probe logged, and where it stood in the capture.
  struct Logged {
    Log::Detail::CapturedLog log;
    Log::Detail::CapturedLog::Mark before;
    Log::Detail::CapturedLog::Mark after;
  };

  // Probes the sorted, distinct records of chunk: announces the reads, then
  // makes them.
  void ProbeChunk(std::span<const ULONGLONG> chunk);

  // Probes one record into the memo. bytes is the record as $MFT holds it, or
  // empty when ParseFileRecord() reads it itself.
  void ProbeOne(ULONGLONG record, std::span<const BYTE> bytes);

  const NtfsVolume<S>* volume_;
  // Reused for every probe, as it clears itself on each parse.
  FileRecord<S> named_;
  // The levels the sinks accept, taken once: a capture MUST NOT ask them.
  Log::Detail::LevelMask levels_;
  std::unordered_map<ULONGLONG, Known> known_;
  std::unordered_map<ULONGLONG, Logged> logs_;
  // Where a run read logs, to be dropped: a run that fails is read again
  // record by record, and logs there.
  Log::Detail::CapturedLog read_log_;
  // The distinct records of the group still to probe.
  std::vector<ULONGLONG> pending_;
  // The bytes of the run being probed.
  std::vector<BYTE> run_bytes_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
