#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <ntfs-browser/namespace.h>

// The constants of the read path that a caller MAY change at run time, and the
// read patterns they serve. Every field defaults to the value the library had
// before the field existed: a default-constructed Settings changes no read, no
// hint and no log line.
//
// A field belongs to one family:
//   A (PrefetchDepth): how far ahead the reads are announced or started. It
//     never changes which bytes are read, only when. Safe to adapt by itself.
//   B (ReadShape): changes the bytes read (block, window, gap, hole, or what a
//     memory budget makes the scan read again). Only an explicit option sets
//     it: no test, fuzz run or recorded trace MAY use a value but the default.
//   C (CpuMemory): threads, chunks, batches and buffers. It follows a curve
//     with no ideal value, and changes no byte read.
// The guard rails that cap a forged input (family D) are not here: they are
// never tuned.
namespace NTFS_BROWSER_NAMESPACE::Tuning {

inline constexpr std::uint64_t kibibyte = std::uint64_t{1} << 10U;
inline constexpr std::uint64_t mebibyte = std::uint64_t{1} << 20U;
inline constexpr std::uint64_t gibibyte = std::uint64_t{1} << 30U;

// The four ways the library reads, each tuned apart.
enum class Pattern : std::uint8_t {
  // M1: large sequential blocks, announced ahead. The first pass over $MFT,
  // the change journal.
  LargeSequential,
  // M2: small scattered reads, grouped and announced. The second pass, the
  // probes of a directory walk.
  ScatteredGrouped,
  // M3: small dependent reads, not announced. A directory record, then its
  // $INDEX_ALLOCATION blocks, one at a time.
  DependentSmall,
  // M4: large blocks of file data. The content check.
  LargeData
};

inline constexpr std::size_t pattern_count = 4;

// "M1" to "M4".
[[nodiscard]] constexpr std::string_view PatternName(Pattern pattern) noexcept {
  constexpr std::array<std::string_view, pattern_count> names{"M1", "M2", "M3",
                                                              "M4"};
  return names.at(static_cast<std::size_t>(pattern));
}

// The families above.
enum class Family : std::uint8_t { PrefetchDepth, ReadShape, CpuMemory };

// How a reader reads ahead. Kept once per pattern, since the best depth of a
// sequential scan is not that of scattered probes.
struct ReaderTuning {
  // A: reads started ahead at once, at most. A SATA SSD needs 32 for small
  // ranges.
  std::size_t max_reads{32};
  // A: bytes the reads ahead hold at most, in flight or waiting for a caller.
  std::uint64_t max_bytes{64 * mebibyte};
  // A: bytes one announced range holds at most. A longer one is not announced.
  std::uint64_t max_range_bytes{16 * mebibyte};
  // A: announced ranges no read has used yet, at most. Past it, a hint is
  // dropped.
  std::size_t max_announced_ranges{1024};
  // A: announced ranges searched for the one a read falls in.
  std::size_t match_lookahead{64};
};

// The scan of $MFT (Mft::MftTree) and the volume metadata it reads.
struct MftTuning {
  // B: bytes of $MFT one window reads, the block of the first pass.
  std::size_t window_bytes{4 * mebibyte};
  // B: free slots a window may span between two used ones.
  std::uint64_t max_gap_slots{64};
  // B: records one read of the second pass covers at most.
  std::size_t max_run_slots{64};
  // B: extension records the second pass reads at once for one base record.
  std::size_t max_preread_extensions{4096};
  // B: bytes of raw records held for the second pass. Past it, records are read
  // again.
  std::uint64_t max_held_record_bytes{sizeof(void*) >= 8 ? gibibyte
                                                         : 128 * mebibyte};
  // B: data-state queries kept before one batch resolves them against the
  // bitmap.
  std::size_t max_pending_ranges{262144};
  // B: bytes of the cluster bitmap kept for the batches of a scan.
  std::uint64_t max_kept_bitmap_bytes{32 * mebibyte};
  // A: windows announced ahead of the one read.
  std::size_t max_announced_windows{64};
  // A: bytes of windows announced ahead of the one read.
  std::uint64_t max_announced_bytes{16 * mebibyte};
  // A: waiting records whose reads the second pass announces ahead.
  std::size_t max_announced_records{64};
  // A: ranges the second pass has announced and not read, at most.
  std::size_t max_announced_ranges{512};
  // A: waiting records with reads between two restarts of the announcements.
  std::size_t announce_restart_interval{1024};
  // A: bytes of one range announced for $J.
  std::uint64_t journal_piece_bytes{4 * mebibyte};
  // C: bytes of one block of held records.
  std::size_t held_block_bytes{4 * mebibyte};
  // C: slots one chunk of a parsed window holds.
  std::size_t parse_chunk_slots{64};
  // C: waiting records one chunk of the second pass holds.
  std::size_t waiting_chunk_records{64};
  // C: waiting records one batch of the second pass holds.
  std::size_t waiting_batch_records{4096};
  // C: threads an automatic count takes at most.
  unsigned max_auto_threads{4};
  // C: slots below which an automatic count stays at one thread.
  std::uint64_t min_auto_threaded_slots{131072};
  // C: slots an automatic scan parses alone before it decides about threads.
  std::uint64_t probe_slots{65536};
  // C: share of the probe, in thousandths, that loading windows may take for
  // threads to pay.
  unsigned max_load_per_mille{200};
  // C: waiting records from which an automatic count threads the second pass.
  std::size_t min_auto_threaded_waiting{2048};
};

// The batched probes of a directory walk (Io::EntryProber).
struct ProbeTuning {
  // B: bytes of $MFT a hole may span between two probed records.
  std::size_t max_gap_bytes{64 * kibibyte};
  // B: bytes of $MFT one read takes at most.
  std::size_t max_run_bytes{mebibyte};
  // B: records probed per round of announce, read and withdraw.
  std::size_t max_chunk_records{128};
  // B: records the memo holds before it is emptied, and probed again.
  std::size_t max_known_records{std::size_t{1} << 18U};
};

// The read ahead of the directories of a recursive listing
// (Io::DirectoryReadAhead).
struct WalkTuning {
  // A: directories read ahead of the one being listed, their records and the
  // first level of their index blocks together. 0 is off, and the listing then
  // reads as it always did. Each costs about 8 KiB while held: a record of 1
  // KiB and one or two index blocks of 4 KiB, the usual size of a directory
  // that has any.
  std::size_t read_ahead_dirs{0};
  // A: bytes of records and index blocks held ahead at most. Past it, nothing
  // is planned until the listing has used some. 16 MiB holds the next two
  // thousand directories of the usual size.
  std::uint64_t read_ahead_bytes{16 * mebibyte};
};

// The block cache of FullCache, and the block FileReader reads.
struct CacheTuning {
  // B: bytes of one block read from the disk and cached.
  std::size_t block_bytes{64 * kibibyte};
};

// The cluster bitmap and the bad cluster list.
struct AllocTuning {
  // B: bytes of the cluster bitmap one window holds.
  std::size_t bitmap_window_bytes{mebibyte};
};

// The content check of the cluster account (M4).
struct CheckTuning {
  // B: bytes one read of the check covers at most.
  std::uint64_t max_read_bytes{4 * mebibyte};
  // B: bytes nobody asks for that one read may span between two it wants.
  std::uint64_t max_gap_bytes{64 * kibibyte};
  // A: reads announced and not made yet, at most.
  std::size_t max_announced_reads{32};
  // A: bytes announced ahead of the read being made.
  std::uint64_t max_announced_bytes{32 * mebibyte};
};

// The change journal loader.
struct UsnTuning {
  // B: bytes asked of the $J stream at a time.
  std::size_t read_chunk_bytes{mebibyte};
};

// The Io::ReadAheadDiskReader that VolumeOptions::read_ahead_bytes turns on.
struct ReadAheadTuning {
  // A: chunks the worker keeps read ahead of the caller. Two make a double
  // buffer: the caller works on one while the worker reads the other.
  std::size_t window_chunks{2};
};

// The buffered listing of the console tools. The library does not read it.
struct OutputTuning {
  // C: bytes of a listing gathered before stdout gets them.
  std::size_t flush_bytes{mebibyte};
};

// Everything a caller MAY tune, in VolumeOptions::tuning.
struct Settings {
  // One per Pattern, in the order of the enum.
  std::array<ReaderTuning, pattern_count> reader{};
  MftTuning mft;
  ProbeTuning probe;
  WalkTuning walk;
  CacheTuning cache;
  AllocTuning alloc;
  CheckTuning check;
  UsnTuning usn;
  ReadAheadTuning readahead;
  OutputTuning output;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tuning
