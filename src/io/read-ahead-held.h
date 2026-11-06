#pragma once

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "io/record-memo.h"
#include "io/record-window.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// The parts of DirectoryReadAhead that hold bytes or share a capture. They
// depend on no strategy.

// Bytes of one block a FullCache reads, as FileReader keeps it: at least one
// sector of the largest size NTFS supports.
[[nodiscard]] inline std::size_t
    CacheBlockBytes(const Tuning::CacheTuning& cache) noexcept {
  constexpr std::size_t min_block_bytes = 4096;
  return (std::max)(cache.block_bytes, min_block_bytes);
}

// The raw bytes that a read ahead holds for the directories it was told about:
// their file records and their index blocks. Counts what it holds and serves
// into the stats it is given.
class HeldStore {
 public:
  explicit HeldStore(DirectoryReadAheadStats& stats) noexcept
      : stats_(&stats) {}

  // Whether a record or a block of directory is held.
  [[nodiscard]] bool Contains(ULONGLONG directory) const {
    return held_.contains(directory);
  }

  // Whether the block vcn of directory is held.
  [[nodiscard]] bool ContainsBlock(ULONGLONG directory, ULONGLONG vcn) const {
    const auto held = held_.find(directory);
    return held != held_.end() && held->second.blocks.contains(vcn);
  }

  // Whether the file record of record is held.
  [[nodiscard]] bool HasRecord(ULONGLONG record) const {
    const auto held = held_.find(record);
    return held != held_.end() && !held->second.record.empty();
  }

  // The held bytes of the file record of record. record MUST be held.
  [[nodiscard]] std::span<const BYTE> RecordOf(ULONGLONG record) const {
    return held_.at(record).record;
  }

  // The bytes of the record the listing asks for, empty when not held. Counts
  // the listing as served, once.
  [[nodiscard]] std::span<const BYTE> ServeRecord(ULONGLONG record) {
    const auto held = held_.find(record);
    if (held == held_.end() || held->second.record.empty()) {
      return {};
    }
    if (!held->second.record_served) {
      held->second.record_served = true;
      stats_->records_served++;
    }
    return held->second.record;
  }

  // The bytes of a block the listing asks for, empty when not held. Counts the
  // listing as served, once.
  [[nodiscard]] std::span<const BYTE> ServeBlock(ULONGLONG directory,
                                                 ULONGLONG vcn) {
    const auto held = held_.find(directory);
    if (held == held_.end()) {
      return {};
    }
    const auto block = held->second.blocks.find(vcn);
    if (block == held->second.blocks.end()) {
      return {};
    }
    if (!block->second.served) {
      block->second.served = true;
      stats_->blocks_served++;
    }
    return block->second.bytes;
  }

  // The bytes of a held block, without counting the listing as served.
  [[nodiscard]] std::span<const BYTE> PeekBlock(ULONGLONG directory,
                                                ULONGLONG vcn) const {
    const auto held = held_.find(directory);
    if (held == held_.end()) {
      return {};
    }
    const auto block = held->second.blocks.find(vcn);
    return block == held->second.blocks.end()
               ? std::span<const BYTE>{}
               : std::span<const BYTE>(block->second.bytes);
  }

  // Holds a copy of the bytes of the file record record.
  void HoldRecord(ULONGLONG record, std::span<const BYTE> bytes) {
    HeldDirectory& held = held_[record];
    held.record.assign(bytes.begin(), bytes.end());
    held_bytes_ += held.record.size();
    NotePeak();
    stats_->records_read++;
  }

  // Holds a copy of the index block vcn of directory.
  void HoldBlock(ULONGLONG directory, ULONGLONG vcn,
                 std::span<const BYTE> bytes) {
    HeldDirectory& held = held_[directory];
    held.blocks[vcn].bytes.assign(bytes.begin(), bytes.end());
    held_bytes_ += bytes.size();
    NotePeak();
    stats_->blocks_read++;
  }

  // Bytes held now.
  [[nodiscard]] std::uint64_t Bytes() const noexcept { return held_bytes_; }

  // Bytes held for directory: its record and its blocks.
  [[nodiscard]] std::uint64_t BytesOf(ULONGLONG directory) const {
    const auto held = held_.find(directory);
    if (held == held_.end()) {
      return 0;
    }
    std::uint64_t bytes = held->second.record.size();
    for (const auto& [vcn, block] : held->second.blocks) {
      bytes += block.bytes.size();
    }
    return bytes;
  }

  // Frees what is held for directory, and counts what the listing never took.
  void Release(ULONGLONG directory) {
    const auto held = held_.find(directory);
    if (held == held_.end()) {
      return;
    }
    if (!held->second.record.empty() && !held->second.record_served) {
      stats_->records_unused++;
    }
    for (const auto& [vcn, block] : held->second.blocks) {
      if (!block.served) {
        stats_->blocks_unused++;
      }
    }
    held_bytes_ -= BytesOf(directory);
    held_.erase(held);
  }

 private:
  // One index block held, and whether the listing took it.
  struct HeldBlock {
    std::vector<BYTE> bytes;
    bool served{false};
  };

  // What is held for one directory.
  struct HeldDirectory {
    // The raw bytes of its file record, empty when only blocks are held.
    std::vector<BYTE> record;
    bool record_served{false};
    // Its index blocks, by sub-node VCN.
    std::unordered_map<ULONGLONG, HeldBlock> blocks;
  };

  void NotePeak() noexcept {
    stats_->peak_held_bytes = (std::max)(stats_->peak_held_bytes, held_bytes_);
  }

  DirectoryReadAheadStats* stats_;
  std::unordered_map<ULONGLONG, HeldDirectory> held_;
  std::uint64_t held_bytes_{0};
};

// The runs of $MFT that the probes and the batches read last, and the few
// bytes of a probe that survive them. Both are off under FullCache, whose
// block cache holds the same bytes.
class RecordKeeper {
 public:
  // keeps_records is false for a volume that has a block cache.
  RecordKeeper(const Tuning::WalkTuning& walk, bool keeps_records,
               std::size_t record_size, DirectoryReadAheadStats& stats)
      : window_(keeps_records ? walk.record_window_bytes : 0, record_size),
        memo_(keeps_records ? walk.record_memo_entries : 0, record_size),
        stats_(&stats) {}

  // Whether the window or the memo holds anything at all.
  [[nodiscard]] bool Enabled() const noexcept {
    return window_.Enabled() || memo_.Enabled();
  }

  // Whether the window is on.
  [[nodiscard]] bool WindowEnabled() const noexcept {
    return window_.Enabled();
  }

  // The bytes of record in the window, empty when it does not hold them.
  [[nodiscard]] std::span<const BYTE> FindInWindow(ULONGLONG record) {
    const std::span<const BYTE> bytes = window_.Find(record);
    if (!bytes.empty()) {
      stats_->window_records++;
    }
    return bytes;
  }

  // A record that a probe may judge: the window's bytes, or the few bytes of a
  // probe that the memo kept of it. Empty when neither holds it.
  [[nodiscard]] std::span<const BYTE> FindForProbe(ULONGLONG record) {
    const std::span<const BYTE> bytes = FindInWindow(record);
    if (!bytes.empty()) {
      return bytes;
    }
    const std::span<const BYTE> kept = memo_.Find(record);
    if (!kept.empty()) {
      stats_->memo_records++;
    }
    return kept;
  }

  // The bytes of record that a probe would find in the window or the memo,
  // without counting the use.
  [[nodiscard]] std::span<const BYTE> FindQuietly(ULONGLONG record) {
    const std::span<const BYTE> bytes = window_.Find(record);
    return bytes.empty() ? memo_.Find(record) : bytes;
  }

  // Gives a run of $MFT that was read to the window and to the memo.
  void KeepRun(ULONGLONG first_record, std::span<const BYTE> bytes) {
    window_.Put(first_record, bytes);
    memo_.Put(first_record, bytes);
  }

 private:
  RecordWindow window_;
  RecordMemo memo_;
  DirectoryReadAheadStats* stats_;
};

// What the batch under way logs, to be dropped: the lines, and the levels the
// sinks accept, taken once since a capture MUST NOT ask them.
struct ReadAheadCapture {
  Log::Detail::LevelMask levels{Log::Detail::EnabledLevels()};
  Log::Detail::CapturedLog log;

  // Forgets the lines held.
  void Reset() noexcept { log.Reset(levels); }
};

// The state the parts of a read ahead share: what it has counted, what it
// holds, and what it captures. Its parts keep pointers into it, so it neither
// copies nor moves.
struct ReadAheadShared {
  ReadAheadShared(const Tuning::WalkTuning& walk, bool keeps_records,
                  std::size_t record_size)
      : store(stats), keeper(walk, keeps_records, record_size, stats) {}

  ReadAheadShared(const ReadAheadShared&) = delete;
  ReadAheadShared& operator=(const ReadAheadShared&) = delete;
  ReadAheadShared(ReadAheadShared&&) = delete;
  ReadAheadShared& operator=(ReadAheadShared&&) = delete;
  ~ReadAheadShared() = default;

  DirectoryReadAheadStats stats;
  HeldStore store;
  RecordKeeper keeper;
  ReadAheadCapture capture;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
