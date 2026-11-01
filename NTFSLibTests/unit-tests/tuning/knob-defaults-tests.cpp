#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/knobs.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t mib = kib * kib;

// The number of knobs: one for each field of Tuning::Settings that the library
// reads, and the two VolumeOptions fields that are tuned with them.
constexpr size_t knob_count = 45;

struct OldConstant {
  std::string_view name;
  std::uint64_t value;
};

// The constants the read path had before Tuning::Settings existed, one for
// each knob. They are written out here and not read from the library, so that
// a changed default shows as a difference. Two are not old: cache.block_bytes
// was 64 KiB until the cache took 4 KiB blocks, and cache.max_run_bytes came
// with that change.
constexpr std::array<OldConstant, knob_count> old_constants{{
    {"reader.max_reads", 32},
    {"reader.max_bytes", 64 * mib},
    {"reader.max_range_bytes", 16 * mib},
    {"reader.max_announced_ranges", 1024},
    {"reader.match_lookahead", 64},
    {"mft.window_bytes", 4 * mib},
    {"mft.max_gap_slots", 64},
    {"mft.max_announced_windows", 64},
    {"mft.max_announced_bytes", 16 * mib},
    {"mft.journal_piece_bytes", 4 * mib},
    {"mft.max_run_slots", 64},
    {"mft.max_preread_extensions", 4096},
    {"mft.max_held_record_bytes", sizeof(void*) >= 8 ? 1024 * mib : 128 * mib},
    {"mft.max_pending_ranges", 262144},
    {"mft.max_kept_bitmap_bytes", 32 * mib},
    {"mft.max_announced_records", 64},
    {"mft.max_announced_ranges", 512},
    {"mft.announce_restart_interval", 1024},
    {"mft.held_block_bytes", 4 * mib},
    {"mft.parse_chunk_slots", 64},
    {"mft.waiting_chunk_records", 64},
    {"mft.waiting_batch_records", 4096},
    {"mft.max_auto_threads", 4},
    {"mft.min_auto_threaded_slots", 131072},
    {"mft.probe_slots", 65536},
    {"mft.max_load_per_mille", 200},
    {"mft.min_auto_threaded_waiting", 2048},
    {"probe.max_gap_bytes", 64 * kib},
    {"probe.max_run_bytes", mib},
    {"probe.max_chunk_records", 128},
    {"probe.max_known_records", std::uint64_t{1} << 18U},
    // The read ahead of a recursive listing: no constant before it, and on.
    {"walk.read_ahead_dirs", 64},
    {"walk.read_ahead_bytes", 16 * mib},
    {"cache.block_bytes", 4 * kib},
    {"cache.max_run_bytes", mib},
    {"cache.max_bytes", 256 * mib},
    {"readahead.chunk_bytes", 0},
    {"readahead.window_chunks", 2},
    {"alloc.bitmap_window_bytes", mib},
    {"check.max_read_bytes", 4 * mib},
    {"check.max_gap_bytes", 64 * kib},
    {"check.max_announced_reads", 32},
    {"check.max_announced_bytes", 32 * mib},
    {"usn.read_chunk_bytes", mib},
    {"output.flush_bytes", mib},
}};

// The patterns a knob serves, in order.
std::vector<Tuning::Pattern> ServedPatterns(const Tuning::Knob& knob) {
  std::vector<Tuning::Pattern> patterns;
  for (size_t i = 0; i < Tuning::pattern_count; i++) {
    if ((knob.patterns & (1U << i)) != 0) {
      patterns.push_back(static_cast<Tuning::Pattern>(i));
    }
  }
  return patterns;
}

}  // namespace

TEST_CASE("The knob table lists 45 knobs with unique names",
          "[tuning][knobs][invariance]") {
  const std::span<const Tuning::Knob> knobs = Tuning::Knobs();

  CHECK(knobs.size() == knob_count);
  std::set<std::string_view> names;
  for (const Tuning::Knob& knob : knobs) {
    CHECK_FALSE(knob.name.empty());
    CHECK(names.insert(knob.name).second);
    CHECK(Tuning::FindKnob(knob.name) == &knob);
  }
  CHECK(names.size() == knob_count);
}

TEST_CASE("The knob table names exactly the knobs the old constants had",
          "[tuning][knobs][invariance]") {
  std::set<std::string_view> table;
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    table.insert(knob.name);
  }
  std::set<std::string_view> expected;
  for (const OldConstant& old : old_constants) {
    expected.insert(old.name);
  }

  CHECK(expected.size() == knob_count);
  CHECK(table == expected);
}

TEST_CASE("Every knob defaults to the constant the library had before",
          "[tuning][knobs][invariance]") {
  for (const OldConstant& old : old_constants) {
    INFO("knob " << old.name);
    const Tuning::Knob* const knob = Tuning::FindKnob(old.name);
    REQUIRE(knob != nullptr);
    CHECK(knob->default_value == old.value);
  }
}

TEST_CASE("Every knob reads its default from a default VolumeOptions",
          "[tuning][knobs][invariance]") {
  const VolumeOptions options;
  const VolumeOptions from_settings{.tuning = Tuning::Settings{}};

  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    INFO("knob " << knob.name);
    const std::vector<Tuning::Pattern> patterns = ServedPatterns(knob);
    REQUIRE_FALSE(patterns.empty());
    CHECK(knob.min_value <= knob.default_value);
    CHECK(knob.default_value <= knob.max_value);
    for (const Tuning::Pattern pattern : patterns) {
      INFO("pattern " << Tuning::PatternName(pattern));
      CHECK(knob.get(options, pattern) == knob.default_value);
      CHECK(knob.get(from_settings, pattern) == knob.default_value);
    }
    if (!knob.per_pattern) {
      continue;
    }
    // A per-pattern knob has the same default for all the patterns.
    for (size_t i = 0; i < Tuning::pattern_count; i++) {
      CHECK(knob.get(options, static_cast<Tuning::Pattern>(i)) ==
            knob.default_value);
    }
  }
}

TEST_CASE("A default Settings holds the old constants in every field",
          "[tuning][knobs][invariance]") {
  const Tuning::Settings settings;

  for (const Tuning::ReaderTuning& reader : settings.reader) {
    CHECK(reader.max_reads == 32);
    CHECK(reader.max_bytes == 64 * mib);
    CHECK(reader.max_range_bytes == 16 * mib);
    CHECK(reader.max_announced_ranges == 1024);
    CHECK(reader.match_lookahead == 64);
  }
  CHECK(settings.mft.window_bytes == 4 * mib);
  CHECK(settings.mft.max_gap_slots == 64);
  CHECK(settings.mft.max_run_slots == 64);
  CHECK(settings.mft.max_preread_extensions == 4096);
  CHECK(settings.mft.max_pending_ranges == 262144);
  CHECK(settings.mft.max_kept_bitmap_bytes == 32 * mib);
  CHECK(settings.mft.max_announced_windows == 64);
  CHECK(settings.mft.max_announced_bytes == 16 * mib);
  CHECK(settings.mft.max_announced_records == 64);
  CHECK(settings.mft.max_announced_ranges == 512);
  CHECK(settings.mft.announce_restart_interval == 1024);
  CHECK(settings.mft.journal_piece_bytes == 4 * mib);
  CHECK(settings.mft.held_block_bytes == 4 * mib);
  CHECK(settings.mft.parse_chunk_slots == 64);
  CHECK(settings.mft.waiting_chunk_records == 64);
  CHECK(settings.mft.waiting_batch_records == 4096);
  CHECK(settings.mft.max_auto_threads == 4);
  CHECK(settings.mft.min_auto_threaded_slots == 131072);
  CHECK(settings.mft.probe_slots == 65536);
  CHECK(settings.mft.max_load_per_mille == 200);
  CHECK(settings.mft.min_auto_threaded_waiting == 2048);
  CHECK(settings.probe.max_gap_bytes == 64 * kib);
  CHECK(settings.probe.max_run_bytes == mib);
  CHECK(settings.probe.max_chunk_records == 128);
  CHECK(settings.probe.max_known_records == (std::uint64_t{1} << 18U));
  CHECK(settings.walk.read_ahead_dirs == 64);
  CHECK(settings.walk.read_ahead_bytes == 16 * mib);
  CHECK(settings.cache.block_bytes == 4 * kib);
  CHECK(settings.cache.max_run_bytes == mib);
  CHECK(settings.alloc.bitmap_window_bytes == mib);
  CHECK(settings.check.max_read_bytes == 4 * mib);
  CHECK(settings.check.max_gap_bytes == 64 * kib);
  CHECK(settings.check.max_announced_reads == 32);
  CHECK(settings.check.max_announced_bytes == 32 * mib);
  CHECK(settings.usn.read_chunk_bytes == mib);
  CHECK(settings.readahead.window_chunks == 2);
  CHECK(settings.output.flush_bytes == mib);
}

TEST_CASE("A default VolumeOptions keeps the values it had before tuning",
          "[tuning][knobs][invariance]") {
  const VolumeOptions options;

  CHECK_FALSE(options.include_deleted);
  CHECK_FALSE(options.recover_errors);
  CHECK(options.max_cache_bytes ==
        NTFS_BROWSER_NAMESPACE::Cache::default_max_cache_bytes);
  CHECK(options.max_cache_bytes == 256 * mib);
  CHECK(options.read_ahead_bytes == 0);
  CHECK(options.max_usn_records ==
        NTFS_BROWSER_NAMESPACE::Usn::default_max_records);
  CHECK(options.max_usn_records == 1638400);
}

TEST_CASE("Every knob says what its value costs in memory", "[tuning][knobs]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    INFO("knob " << knob.name);
    const std::string_view name = knob.name;
    // The largest value the search can try never wraps the cost.
    if (knob.memory_unit_bytes != 0) {
      CHECK(knob.max_value <=
            std::numeric_limits<std::uint64_t>::max() / knob.memory_unit_bytes);
    }
    const bool is_gap = name.find("gap") != std::string_view::npos;
    const bool is_threshold = name.find("min_") != std::string_view::npos ||
                              name.find("load") != std::string_view::npos ||
                              name.find("lookahead") != std::string_view::npos;
    if (is_gap || is_threshold) {
      CHECK(knob.memory_unit_bytes == 0);
    } else if (name.ends_with("_bytes") && name != "reader.max_range_bytes") {
      CHECK(knob.memory_unit_bytes == 1);
    }
  }
  const auto unit = [](std::string_view name) {
    const Tuning::Knob* const knob = Tuning::FindKnob(name);
    REQUIRE(knob != nullptr);
    return knob->memory_unit_bytes;
  };
  CHECK(unit("reader.max_bytes") == 1);
  CHECK(unit("mft.window_bytes") == 1);
  CHECK(unit("mft.max_gap_slots") == 0);
  CHECK(unit("mft.announce_restart_interval") == 0);
  CHECK(unit("mft.max_run_slots") > 1);
  CHECK(unit("reader.max_reads") > 1);
}
