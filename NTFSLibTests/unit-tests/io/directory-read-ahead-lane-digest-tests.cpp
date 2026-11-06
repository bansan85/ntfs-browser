#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "directory-read-ahead-support.h"
#include "fake-ntfs-image.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::CountingDiskReader;

namespace {

// What is pinned about the lane calls of one listing.
struct LaneDigest {
  std::string_view name;
  // FNV-1a over the kind, offset and length of every lane call.
  std::uint64_t lane_hash;
  std::uint64_t lane_calls;
  // Calls the listing made that are not the lane's.
  std::uint64_t other_calls;
  std::uint64_t runs_announced;
  std::uint64_t runs_withdrawn;
  // FNV-1a over every counter of DirectoryReadAheadStats.
  std::uint64_t stats_hash;
};

// The digests recorded from the code of commit e00a529, before the lane calls
// were changed. The hidden case "write the lane digests" writes the file.
constexpr LaneDigest recorded_digests[] = {
#include "directory-read-ahead-lane-digests.inc"
};

// FNV-1a, 64 bits: the offset basis and the prime.
constexpr std::uint64_t fnv_basis = 0xCBF29CE484222325ULL;
constexpr std::uint64_t fnv_prime = 0x100000001B3ULL;

// Folds the eight bytes of value into hash, least significant first, so that
// the digest does not depend on the platform.
#ifdef __clang__
// The product wraps on purpose, as FNV does.
__attribute__((no_sanitize("unsigned-integer-overflow")))
#endif
void Fold(std::uint64_t& hash, std::uint64_t value) {
  // Bits in a byte.
  constexpr unsigned byte_bits = 8;
  // Bytes in a 64-bit value.
  constexpr unsigned byte_count = 8;
  for (unsigned byte = 0; byte < byte_count; byte++) {
    hash ^= (value >> (byte * byte_bits)) & 0xFFU;
    hash *= fnv_prime;
  }
}

std::uint64_t LaneHash(const std::vector<CountingDiskReader::Call>& calls) {
  std::uint64_t hash = fnv_basis;
  for (const CountingDiskReader::Call& call : calls) {
    Fold(hash, static_cast<std::uint64_t>(call.kind));
    Fold(hash, call.offset);
    Fold(hash, call.length);
  }
  return hash;
}

std::uint64_t StatsHash(const Io::DirectoryReadAheadStats& stats) {
  std::uint64_t hash = fnv_basis;
  for (const std::uint64_t counter :
       {stats.held_bytes, stats.peak_held_bytes, stats.records_read,
        stats.records_served, stats.blocks_read, stats.blocks_served,
        stats.records_unused, stats.blocks_unused, stats.window_records,
        stats.memo_records, stats.batches, stats.runs_planned,
        stats.runs_announced, stats.runs_withdrawn, stats.runs_read,
        stats.runs_unplanned, stats.runs_window_held}) {
    Fold(hash, counter);
  }
  return hash;
}

// One listing to pin: a tree, a cache strategy, the options of the volume and
// of the reader.
struct Scenario {
  std::string name;
  const std::vector<BYTE>* image{nullptr};
  bool full_cache{false};
  VolumeOptions options;
  Tuning::MediaKind kind{Tuning::MediaKind::Unknown};
  bool raw_callback{false};
};

// What a scenario sets on top of the defaults.
struct Knobs {
  std::size_t runs{64};
  std::size_t dirs{64};
  std::uint64_t window_bytes{Tuning::WalkTuning{}.record_window_bytes};
  std::uint64_t memo_entries{Tuning::WalkTuning{}.record_memo_entries};
  // Every probe a run of its own, however close two records lie.
  bool tight{false};
};

VolumeOptions Tuned(const Knobs& knobs) {
  VolumeOptions options;
  options.tuning.walk.read_ahead_runs = knobs.runs;
  options.tuning.walk.read_ahead_dirs = knobs.dirs;
  options.tuning.walk.record_window_bytes = knobs.window_bytes;
  options.tuning.walk.record_memo_entries = knobs.memo_entries;
  if (knobs.tight) {
    options.tuning.probe.max_gap_bytes = 0;
    options.tuning.probe.max_chunk_records = 1;
  }
  return options;
}

// The trees of the scenarios. A std::deque keeps the images where they are.
struct Trees {
  std::deque<std::vector<BYTE>> images;
  std::vector<ReadAhead::TreeFixture> fixtures;

  const std::vector<BYTE>& Add(const std::vector<Tests::FakeTreeNode>& nodes) {
    images.push_back(Tests::BuildFakeNtfsImageWithNameTree(nodes));
    return images.back();
  }
};

const char* StrategyName(bool full_cache) {
  return full_cache ? "FullCache" : "NoCache";
}

// A tree of the scenarios, and whether its probes are tight: records side by
// side or two apart would otherwise share a run.
struct NamedTree {
  std::string name;
  const std::vector<BYTE>* image;
  bool tight;
};

// Every scenario. Few and rich trees, each listed under the settings that
// change which lane calls are made. Each name is unique.
const std::vector<Scenario>& Scenarios() {
  static const Trees trees = [] {
    Trees built;
    // Many small directories: the shape of a system volume.
    built.Add(ReadAhead::GroupedTree(4, 24));
    // A stack that grows and shrinks: 3 + 9 + 27 + 81 directories.
    built.Add(ReadAhead::BranchingTree(3, 4));
    // Directories whose probes lie a run of $MFT apart: 36 runs each.
    built.Add(ReadAhead::ScatteredTree(3, 36, 70));
    // Records side by side, and two apart (closer than a block of the cache).
    built.Add(ReadAhead::ScatteredTree(4, 36, 0));
    built.Add(ReadAhead::ScatteredTree(4, 36, 2));
    // One directory of 300 probes, each a run of its own.
    built.Add(ReadAhead::ScatteredTree(1, 44, 0));
    built.Add(ReadAhead::BigDirectoryTree(6));
    built.Add(ReadAhead::SpacedTree(44, 6, 0));
    // Records with two names in one directory: a hard link or a DOS alias.
    built.Add(ReadAhead::LinkedTree(3, 20));
    built.fixtures = ReadAhead::Fixtures();
    return built;
  }();

  static const std::vector<Scenario> scenarios = [] {
    std::vector<Scenario> list;
    const auto add =
        [&list](const std::string& name, const std::vector<BYTE>& image,
                bool full_cache, const VolumeOptions& options,
                Tuning::MediaKind kind = Tuning::MediaKind::Unknown,
                bool raw_callback = false) {
          list.push_back({.name = name + "/" + StrategyName(full_cache),
                          .image = &image,
                          .full_cache = full_cache,
                          .options = options,
                          .kind = kind,
                          .raw_callback = raw_callback});
        };
    const std::vector<NamedTree> named{
        {"grouped", &trees.images.at(0), false},
        {"branching", &trees.images.at(1), false},
        {"scattered", &trees.images.at(2), false},
        {"side by side", &trees.images.at(3), true},
        {"two apart", &trees.images.at(4), true},
        {"wide directory", &trees.images.at(5), true},
        {"big", &trees.images.at(6), false},
        {"spaced", &trees.images.at(7), false},
        {"linked", &trees.images.at(8), false}};
    constexpr std::size_t grouped = 0;
    constexpr std::size_t branching = 1;
    constexpr std::size_t scattered = 2;
    constexpr std::size_t side_by_side = 3;
    constexpr std::size_t two_apart = 4;
    constexpr std::size_t wide = 5;
    constexpr std::size_t linked = 8;

    for (const bool full : {false, true}) {
      for (const NamedTree& tree : named) {
        // The defaults.
        add(tree.name + "/default", *tree.image, full, Tuned({}));
        if (tree.tight) {
          add(tree.name + "/default tight", *tree.image, full,
              Tuned({.tight = true}));
        }
      }

      // How many runs the lane keeps: fewer than the plans hold, so that the
      // stack turns over, and more than there are runs at all.
      for (std::size_t index = 0; index <= wide; index++) {
        const NamedTree& tree = named.at(index);
        for (const std::size_t runs : {1U, 2U, 3U, 4096U}) {
          add(tree.name + "/runs=" + std::to_string(runs), *tree.image, full,
              Tuned({.runs = runs, .tight = tree.tight}));
        }
      }
      // Records so close that a block holds both, with the default gap.
      for (const std::size_t index : {side_by_side, two_apart}) {
        add(named.at(index).name + "/runs=3 loose", *named.at(index).image,
            full, Tuned({.runs = 3}));
      }

      // A record named twice is probed once, until the memory of probed
      // records is full and starts again.
      for (const std::uint64_t known : {4U, 64U}) {
        VolumeOptions options = Tuned({.runs = 3});
        options.tuning.probe.max_known_records = known;
        add("linked/known=" + std::to_string(known), *named.at(linked).image,
            full, options);
        options.tuning.probe.max_chunk_records = 4;
        add("linked/known=" + std::to_string(known) + " chunks=4",
            *named.at(linked).image, full, options);
      }

      // How many directories are planned.
      for (const std::size_t index : {grouped, branching, side_by_side}) {
        const NamedTree& tree = named.at(index);
        for (const std::size_t dirs : {1U, 4U}) {
          add(tree.name + "/dirs=" + std::to_string(dirs), *tree.image, full,
              Tuned({.runs = 3, .dirs = dirs, .tight = tree.tight}));
        }
      }
    }

    // The record window and the memo, small or off. FullCache keeps neither.
    struct Memory {
      const char* name;
      std::uint64_t window;
      std::uint64_t memo;
    };
    for (const std::size_t index : {grouped, branching, side_by_side, wide}) {
      const NamedTree& tree = named.at(index);
      for (const Memory& memory :
           {Memory{"no memo", Knobs{}.window_bytes, 0},
            Memory{"no window no memo", 0, 0}, Memory{"small window", 8192, 16},
            Memory{"window 64 KiB", 65536, 16}}) {
        for (const std::size_t runs : {1U, 3U}) {
          add(tree.name + "/" + memory.name + "/runs=" + std::to_string(runs),
              *tree.image, false,
              Tuned({.runs = runs,
                     .window_bytes = memory.window,
                     .memo_entries = memory.memo,
                     .tight = index == wide}));
        }
      }
    }

    // Every tree of the other tests, under each way to open the volume.
    for (const bool full : {false, true}) {
      for (const ReadAhead::TreeFixture& fixture : trees.fixtures) {
        std::size_t mode = 0;
        for (const VolumeOptions& options : ReadAhead::AllOptions()) {
          add(fixture.name + "/mode=" + std::to_string(mode++), fixture.image,
              full, options);
        }
      }
    }

    // Freed items and recovery, with a lane that holds few runs.
    for (const bool full : {false, true}) {
      VolumeOptions options = Tuned({.runs = 5, .dirs = 8});
      options.include_deleted = true;
      options.recover_errors = true;
      for (const std::size_t index : {grouped, branching, scattered}) {
        add(named.at(index).name + "/deleted recover", *named.at(index).image,
            full, options);
      }
    }

    // A raw attribute callback turns the lane off. So does a hard disk, unless
    // its own number of runs says otherwise.
    for (const bool full : {false, true}) {
      const std::vector<BYTE>& image = *named.at(grouped).image;
      add("grouped/raw callback", image, full, Tuned({}),
          Tuning::MediaKind::Unknown, true);
      add("grouped/rotational", image, full, Tuned({}),
          Tuning::MediaKind::Rotational);
      VolumeOptions rotational = Tuned({});
      rotational.tuning.walk.read_ahead_runs_rotational = 8;
      add("grouped/rotational runs=8", image, full, rotational,
          Tuning::MediaKind::Rotational);
      add("scattered/solid state", *named.at(scattered).image, full,
          Tuned({.runs = 8}), Tuning::MediaKind::SolidState);
    }
    return list;
  }();
  return scenarios;
}

// What a scenario did, digested.
struct Outcome {
  LaneDigest digest;
  Io::DirectoryReadAheadStats stats;
  std::vector<CountingDiskReader::Call> lane;
};

Outcome Run(const Scenario& scenario) {
  const ReadAhead::VerifiedLaneZone verified;
  const ReadAhead::LaneRun run =
      scenario.full_cache ? ReadAhead::ListForLane<Cache::Strategy::FullCache>(
                                *scenario.image, scenario.options,
                                scenario.kind, scenario.raw_callback)
                          : ReadAhead::ListForLane<Cache::Strategy::NoCache>(
                                *scenario.image, scenario.options,
                                scenario.kind, scenario.raw_callback);
  Outcome outcome;
  outcome.digest = {.name = scenario.name,
                    .lane_hash = LaneHash(run.lane),
                    .lane_calls = run.lane.size(),
                    .other_calls = run.others.size(),
                    .runs_announced = run.stats.runs_announced,
                    .runs_withdrawn = run.stats.runs_withdrawn,
                    .stats_hash = StatsHash(run.stats)};
  outcome.stats = run.stats;
  outcome.lane = run.lane;
  return outcome;
}

// Every scenario, listed once for all the cases below.
const std::vector<Outcome>& Outcomes() {
  static const std::vector<Outcome> outcomes = [] {
    std::vector<Outcome> list;
    for (const Scenario& scenario : Scenarios()) {
      list.push_back(Run(scenario));
    }
    return list;
  }();
  return outcomes;
}

}  // namespace

TEST_CASE("The lane calls of every listing are the recorded ones",
          "[io][directory-read-ahead][standing-lane][lane-digest]") {
  std::map<std::string_view, const LaneDigest*> recorded;
  for (const LaneDigest& digest : recorded_digests) {
    CHECK(recorded.emplace(digest.name, &digest).second);
  }
  CHECK(recorded.size() == Scenarios().size());

  for (const Outcome& got : Outcomes()) {
    INFO(got.digest.name);
    const auto found = recorded.find(got.digest.name);
    REQUIRE(found != recorded.end());
    const LaneDigest& want = *found->second;
    CHECK(got.digest.lane_calls == want.lane_calls);
    CHECK(got.digest.runs_announced == want.runs_announced);
    CHECK(got.digest.runs_withdrawn == want.runs_withdrawn);
    CHECK(got.digest.other_calls == want.other_calls);
    CHECK(got.digest.lane_hash == want.lane_hash);
    CHECK(got.digest.stats_hash == want.stats_hash);
  }
}

TEST_CASE("The pinned listings reach every path of the lane",
          "[io][directory-read-ahead][standing-lane][lane-digest]") {
  std::size_t with_lane = 0;
  std::size_t withdrawing = 0;
  std::size_t window_held = 0;
  std::size_t unplanned = 0;
  std::size_t kept_back = 0;
  std::size_t longer_than_lane = 0;
  std::size_t most_planned = 0;
  for (std::size_t index = 0; index < Scenarios().size(); index++) {
    const Scenario& scenario = Scenarios().at(index);
    const Io::DirectoryReadAheadStats& stats = Outcomes().at(index).stats;
    INFO(scenario.name);
    const bool lane_expected =
        !scenario.raw_callback &&
        (scenario.kind != Tuning::MediaKind::Rotational ||
         scenario.options.tuning.walk.read_ahead_runs_rotational != 0);
    if (!lane_expected) {
      CHECK(Outcomes().at(index).lane.empty());
      CHECK(stats.runs_planned == 0);
    }
    with_lane += Outcomes().at(index).lane.empty() ? 0 : 1;
    withdrawing += stats.runs_withdrawn != 0 ? 1 : 0;
    window_held += stats.runs_window_held != 0 ? 1 : 0;
    unplanned += stats.runs_unplanned != 0 ? 1 : 0;
    // Planned runs that the lane never announced: the window held them, or an
    // overlap kept them off the lane until the other run was read.
    kept_back +=
        scenario.full_cache && stats.runs_announced < stats.runs_planned ? 1
                                                                         : 0;
    longer_than_lane +=
        stats.runs_planned > 2 * scenario.options.tuning.walk.read_ahead_runs
            ? 1
            : 0;
    most_planned =
        (std::max)(most_planned, static_cast<std::size_t>(stats.runs_planned));
  }
  CHECK(with_lane > 100);
  CHECK(withdrawing > 20);
  CHECK(window_held > 0);
  CHECK(unplanned > 10);
  CHECK(kept_back > 0);
  CHECK(longer_than_lane > 30);
  CHECK(most_planned >= 100);
}

TEST_CASE("write the lane digests",
          "[.write-lane-digests][io][directory-read-ahead]") {
  const char* const path = std::getenv("NTFS_BROWSER_LANE_DIGEST_FILE");
  if (path == nullptr) {
    SKIP("NTFS_BROWSER_LANE_DIGEST_FILE is not set");
  }
  // Built whole first: a scenario that throws leaves the old table as it is.
  std::string table;
  for (const Outcome& outcome : Outcomes()) {
    const LaneDigest& digest = outcome.digest;
    std::array<char, 512> line{};
    std::snprintf(line.data(), line.size(),
                  "    {\"%s\", 0x%016llxULL, %llu, %llu, %llu, %llu, "
                  "0x%016llxULL},\n",
                  std::string(digest.name).c_str(),
                  static_cast<unsigned long long>(digest.lane_hash),
                  static_cast<unsigned long long>(digest.lane_calls),
                  static_cast<unsigned long long>(digest.other_calls),
                  static_cast<unsigned long long>(digest.runs_announced),
                  static_cast<unsigned long long>(digest.runs_withdrawn),
                  static_cast<unsigned long long>(digest.stats_hash));
    table += line.data();
  }
  std::FILE* const file = std::fopen(path, "wb");
  REQUIRE(file != nullptr);
  CHECK(std::fwrite(table.data(), 1, table.size(), file) == table.size());
  CHECK(std::fclose(file) == 0);
}
