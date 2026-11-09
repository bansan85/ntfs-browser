#include "tuning/knobs.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <ntfs-browser/namespace.h>

#include "tuning/tuning.h"
#include "volume-options.h"

namespace NTFS_BROWSER_NAMESPACE::Tuning {

namespace {

// The bit of a Pattern in Knob::patterns.
constexpr std::uint8_t Bit(Pattern pattern) noexcept {
  return static_cast<std::uint8_t>(1U << static_cast<unsigned>(pattern));
}

constexpr std::uint8_t m1 = Bit(Pattern::LargeSequential);
constexpr std::uint8_t m2 = Bit(Pattern::ScatteredGrouped);
constexpr std::uint8_t m3 = Bit(Pattern::DependentSmall);
constexpr std::uint8_t m4 = Bit(Pattern::LargeData);
constexpr std::uint8_t all_patterns = m1 | m2 | m3 | m4;

// Largest value any size knob accepts: 1 TiB, far above a useful buffer, and
// small enough that no product of two of them wraps.
constexpr std::uint64_t huge = std::uint64_t{1} << 40U;

// 2^16, 2^20, 2^24, 2^28 and 2^30: the usual upper bounds of a count.
constexpr std::uint64_t k64k = std::uint64_t{1} << 16U;
constexpr std::uint64_t k1m = std::uint64_t{1} << 20U;
constexpr std::uint64_t k16m = std::uint64_t{1} << 24U;
constexpr std::uint64_t k256m = std::uint64_t{1} << 28U;
constexpr std::uint64_t k1g = std::uint64_t{1} << 30U;

// Values of Knob::memory_unit_bytes: the bytes of memory that one unit of a
// knob's value holds or has in flight. The units of a count are estimates for
// the usual volume. They only rank values of one knob, so they need not be
// exact.
//
// The value holds no memory: a gap, a depth of search, a threshold.
constexpr std::uint32_t no_memory = 0;
// The value is a number of bytes.
constexpr std::uint32_t is_bytes = 1;
// A count of file records of 1 KiB, the usual size.
constexpr std::uint32_t per_record = 1024;
// A count of parsed slots or waiting records, each held as an entry with its
// names and counters until the scan merges it.
constexpr std::uint32_t per_outcome = 512;
// A count of list entries (ranges, queries, memo records): a few words each.
constexpr std::uint32_t per_entry = 64;
// A count of reads in flight: a read buffer of 1 MiB, the size of the reads
// that fill the default byte budget of the reader.
constexpr std::uint32_t per_read = 1U << 20U;
// A count of blocks or chunks of 4 MiB, the default window of $MFT.
constexpr std::uint32_t per_large_block = 4U << 20U;
// A count of directories read ahead: a record of 1 KiB and one or two index
// blocks of 4 KiB, the usual size of a directory that has any.
constexpr std::uint32_t per_directory = 8192;
// A count of probe runs announced ahead: the buffer of a run of a few records
// of $MFT, about 16 KiB.
constexpr std::uint32_t per_probe_run = 16384;
// A count of threads: the stack and the buffers of a parse thread, about 1 MiB.
constexpr std::uint32_t per_thread = 1U << 20U;

// value as a T, cut at the largest T holds: a size_t of a 32-bit build is
// narrower than the 64-bit values a profile gives.
template <typename T>
T Clamped(std::uint64_t value) noexcept {
  return static_cast<T>(
      (std::min<std::uint64_t>)(value, (std::numeric_limits<T>::max)()));
}

// The knob for a field of VolumeOptions::tuning that no pattern splits. FIELD
// is the member path below tuning, for example mft.window_bytes.
#define NTFS_KNOB(NAME, FAMILY, PATTERNS, FIELD, MIN, MAX, UNIT)            \
  Knob {                                                                    \
    NAME, Family::FAMILY, PATTERNS, false, MIN, MAX,                        \
        static_cast<std::uint64_t>(Settings{}.FIELD), UNIT,                 \
        [](const VolumeOptions& options, Pattern) {                         \
          return static_cast<std::uint64_t>(options.tuning.FIELD);          \
        },                                                                  \
        [](VolumeOptions& options, Pattern, std::uint64_t value) {          \
          options.tuning.FIELD =                                            \
              Clamped<std::remove_cvref_t<decltype(options.tuning.FIELD)>>( \
                  value);                                                   \
        }                                                                   \
  }

// The knob for a field of ReaderTuning, kept once per pattern.
#define NTFS_READER_KNOB(NAME, FIELD, MIN, MAX, UNIT)                      \
  Knob {                                                                   \
    NAME, Family::PrefetchDepth, all_patterns, true, MIN, MAX,             \
        static_cast<std::uint64_t>(ReaderTuning{}.FIELD), UNIT,            \
        [](const VolumeOptions& options, Pattern pattern) {                \
          return static_cast<std::uint64_t>(                               \
              options.tuning.reader.at(static_cast<std::size_t>(pattern))  \
                  .FIELD);                                                 \
        },                                                                 \
        [](VolumeOptions& options, Pattern pattern, std::uint64_t value) { \
          auto& field =                                                    \
              options.tuning.reader.at(static_cast<std::size_t>(pattern))  \
                  .FIELD;                                                  \
          field = Clamped<std::remove_cvref_t<decltype(field)>>(value);    \
        }                                                                  \
  }

// A knob of VolumeOptions itself, outside tuning.
#define NTFS_OPTION_KNOB(NAME, FAMILY, PATTERNS, FIELD, MIN, MAX, UNIT) \
  Knob {                                                                \
    NAME, Family::FAMILY, PATTERNS, false, MIN, MAX,                    \
        static_cast<std::uint64_t>(VolumeOptions{}.FIELD), UNIT,        \
        [](const VolumeOptions& options, Pattern) {                     \
          return static_cast<std::uint64_t>(options.FIELD);             \
        },                                                              \
        [](VolumeOptions& options, Pattern, std::uint64_t value) {      \
          options.FIELD = value;                                        \
        }                                                               \
  }

constexpr std::size_t knob_count = 51;

const std::array<Knob, knob_count>& Table() {
  static const std::array<Knob, knob_count> table{{
      NTFS_READER_KNOB("reader.max_reads", max_reads, 1, 256, per_read),
      NTFS_READER_KNOB("reader.max_bytes", max_bytes, 4096, huge, is_bytes),
      NTFS_READER_KNOB("reader.max_range_bytes", max_range_bytes, 4096, huge,
                       no_memory),
      NTFS_READER_KNOB("reader.max_announced_ranges", max_announced_ranges, 1,
                       k1m, per_entry),
      NTFS_READER_KNOB("reader.match_lookahead", match_lookahead, 1, k64k,
                       no_memory),

      NTFS_KNOB("mft.window_bytes", ReadShape, m1, mft.window_bytes, 4096, huge,
                is_bytes),
      NTFS_KNOB("mft.max_gap_slots", ReadShape, m1, mft.max_gap_slots, 0, k16m,
                no_memory),
      NTFS_KNOB("mft.max_announced_windows", PrefetchDepth, m1,
                mft.max_announced_windows, 1, k64k, per_large_block),
      NTFS_KNOB("mft.max_announced_bytes", PrefetchDepth, m1,
                mft.max_announced_bytes, 4096, huge, is_bytes),
      NTFS_KNOB("mft.journal_piece_bytes", PrefetchDepth, m1,
                mft.journal_piece_bytes, 4096, huge, is_bytes),
      NTFS_KNOB("mft.max_run_slots", ReadShape, m2, mft.max_run_slots, 1, k1m,
                per_record),
      NTFS_KNOB("mft.max_preread_extensions", ReadShape, m2,
                mft.max_preread_extensions, 0, k16m, per_record),
      NTFS_KNOB("mft.max_held_record_bytes", ReadShape, m2,
                mft.max_held_record_bytes, 0, huge, is_bytes),
      NTFS_KNOB("mft.max_pending_ranges", ReadShape, m2, mft.max_pending_ranges,
                1, k256m, per_entry),
      NTFS_KNOB("mft.max_kept_bitmap_bytes", ReadShape, m2,
                mft.max_kept_bitmap_bytes, 0, huge, is_bytes),
      NTFS_KNOB("mft.max_announced_records", PrefetchDepth, m2,
                mft.max_announced_records, 1, k1m, per_record),
      NTFS_KNOB("mft.max_announced_ranges", PrefetchDepth, m2,
                mft.max_announced_ranges, 1, k1m, per_entry),
      NTFS_KNOB("mft.announce_restart_interval", PrefetchDepth, m2,
                mft.announce_restart_interval, 1, k16m, no_memory),
      NTFS_KNOB("mft.held_block_bytes", CpuMemory, m1 | m2,
                mft.held_block_bytes, 4096, huge, is_bytes),
      NTFS_KNOB("mft.parse_chunk_slots", CpuMemory, m1, mft.parse_chunk_slots,
                1, k1m, per_outcome),
      NTFS_KNOB("mft.waiting_chunk_records", CpuMemory, m2,
                mft.waiting_chunk_records, 1, k1m, per_outcome),
      NTFS_KNOB("mft.waiting_batch_records", CpuMemory, m2,
                mft.waiting_batch_records, 1, k16m, per_outcome),
      NTFS_KNOB("mft.max_auto_threads", CpuMemory, m1 | m2,
                mft.max_auto_threads, 1, 64, per_thread),
      NTFS_KNOB("mft.min_auto_threaded_slots", CpuMemory, m1,
                mft.min_auto_threaded_slots, 0, huge, no_memory),
      NTFS_KNOB("mft.probe_slots", CpuMemory, m1, mft.probe_slots, 1, huge,
                no_memory),
      NTFS_KNOB("mft.max_load_per_mille", CpuMemory, m1, mft.max_load_per_mille,
                0, 1000, no_memory),
      NTFS_KNOB("mft.min_auto_threaded_waiting", CpuMemory, m2,
                mft.min_auto_threaded_waiting, 0, huge, no_memory),

      // The records that the read ahead of a recursive listing fetches (M3)
      // are planned with the same gaps and runs as the probes (M2).
      NTFS_KNOB("probe.max_gap_bytes", ReadShape, m2 | m3, probe.max_gap_bytes,
                0, huge, no_memory),
      NTFS_KNOB("probe.wide_gap_bytes", ReadShape, m2 | m3,
                probe.wide_gap_bytes, 0, huge, no_memory),
      NTFS_KNOB("probe.wide_gap_min_share_per_mille", ReadShape, m2 | m3,
                probe.wide_gap_min_share_per_mille, 0, 1000, no_memory),
      NTFS_KNOB("probe.max_run_bytes", ReadShape, m2 | m3, probe.max_run_bytes,
                4096, huge, is_bytes),
      NTFS_KNOB("probe.max_chunk_records", ReadShape, m2 | m3,
                probe.max_chunk_records, 1, k1m, per_record),
      NTFS_KNOB("probe.max_known_records", ReadShape, m2,
                probe.max_known_records, 1, k1g, per_entry),

      NTFS_KNOB("walk.read_ahead_dirs", PrefetchDepth, m3, walk.read_ahead_dirs,
                0, 4096, per_directory),
      NTFS_KNOB("walk.read_ahead_bytes", PrefetchDepth, m3,
                walk.read_ahead_bytes, 64 * 1024, k1g, is_bytes),
      NTFS_KNOB("walk.record_window_bytes", ReadShape, m3,
                walk.record_window_bytes, 0, k1g, is_bytes),
      NTFS_KNOB("walk.record_memo_entries", ReadShape, m3,
                walk.record_memo_entries, 0, k1m * 4, per_entry),
      NTFS_KNOB("walk.read_ahead_runs", PrefetchDepth, m3, walk.read_ahead_runs,
                0, 4096, per_probe_run),
      NTFS_KNOB("walk.read_ahead_runs_rotational", PrefetchDepth, m3,
                walk.read_ahead_runs_rotational, 0, 4096, per_probe_run),

      NTFS_KNOB("cache.block_bytes", ReadShape, m2 | m3, cache.block_bytes,
                4096, k256m, is_bytes),
      NTFS_KNOB("cache.max_run_bytes", ReadShape, m2 | m3, cache.max_run_bytes,
                4096, huge, is_bytes),
      NTFS_OPTION_KNOB("cache.max_bytes", CpuMemory, m2 | m3, max_cache_bytes,
                       0, huge, is_bytes),
      NTFS_OPTION_KNOB("readahead.chunk_bytes", ReadShape, m1, read_ahead_bytes,
                       0, std::uint64_t{1} << 26U, is_bytes),

      NTFS_KNOB("readahead.window_chunks", PrefetchDepth, m1,
                readahead.window_chunks, 1, 64, per_large_block),

      NTFS_KNOB("alloc.bitmap_window_bytes", ReadShape, m2,
                alloc.bitmap_window_bytes, 4096, huge, is_bytes),

      NTFS_KNOB("check.max_read_bytes", ReadShape, m4, check.max_read_bytes,
                4096, huge, is_bytes),
      NTFS_KNOB("check.max_gap_bytes", ReadShape, m4, check.max_gap_bytes, 0,
                huge, no_memory),
      NTFS_KNOB("check.max_announced_reads", PrefetchDepth, m4,
                check.max_announced_reads, 1, k64k, per_large_block),
      NTFS_KNOB("check.max_announced_bytes", PrefetchDepth, m4,
                check.max_announced_bytes, 4096, huge, is_bytes),

      NTFS_KNOB("usn.read_chunk_bytes", ReadShape, m1, usn.read_chunk_bytes,
                4096, k256m, is_bytes),
      NTFS_KNOB("output.flush_bytes", CpuMemory, m1 | m2 | m3,
                output.flush_bytes, 4096, k1g, is_bytes),
  }};
  return table;
}

#undef NTFS_KNOB
#undef NTFS_READER_KNOB
#undef NTFS_OPTION_KNOB

}  // namespace

std::span<const Knob> Knobs() { return Table(); }

const Knob* FindKnob(std::string_view name) {
  const std::span<const Knob> knobs = Knobs();
  const auto found = std::ranges::find(knobs, name, &Knob::name);
  return found == knobs.end() ? nullptr : &*found;
}

std::string_view FamilyName(Family family) {
  switch (family) {
    case Family::PrefetchDepth:
      return "A";
    case Family::ReadShape:
      return "B";
    case Family::CpuMemory:
      return "C";
  }
  return "?";
}

std::string ApplyKnob(VolumeOptions& options, std::string_view name,
                      std::uint64_t value, std::optional<Pattern> pattern) {
  const Knob* knob = FindKnob(name);
  if (knob == nullptr) {
    return "unknown key \"" + std::string(name) + "\"";
  }
  if (value < knob->min_value || value > knob->max_value) {
    return "\"" + std::string(name) + "\" is " + std::to_string(value) +
           ", outside " + std::to_string(knob->min_value) + " to " +
           std::to_string(knob->max_value);
  }
  if (pattern && (knob->patterns & Bit(*pattern)) == 0) {
    return "\"" + std::string(name) + "\" does not serve pattern " +
           std::string(PatternName(*pattern));
  }
  if (knob->per_pattern) {
    if (pattern) {
      knob->set(options, *pattern, value);
    } else {
      for (std::size_t index = 0; index < pattern_count; index++) {
        knob->set(options, static_cast<Pattern>(index), value);
      }
    }
  } else {
    knob->set(options, pattern.value_or(Pattern::LargeSequential), value);
  }
  return {};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tuning
