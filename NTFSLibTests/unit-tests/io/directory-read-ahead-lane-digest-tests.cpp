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
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "directory-read-ahead-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using TestDiskReaders::CountingDiskReader;

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

  const std::vector<BYTE>&
      Add(const std::vector<TestFakeImage::FakeTreeNode>& nodes) {
    images.push_back(TestFakeImage::BuildFakeNtfsImageWithNameTree(nodes));
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

// Indexes of the trees in `ScenarioSet::named_`.
enum TreeIndex : std::size_t {
  grouped,
  branching,
  scattered,
  side_by_side,
  two_apart,
  wide,
  big,
  spaced,
  linked
};

// The record window and the memo, small or off.
struct Memory {
  const char* name;
  std::uint64_t window;
  std::uint64_t memo;
};

// Collects the scenarios, each listed under the settings that change which
// lane calls are made.
class ScenarioSet {
 public:
  explicit ScenarioSet(const Trees& trees)
      : trees_(trees),
        named_{{"grouped", &trees.images.at(grouped), false},
               {"branching", &trees.images.at(branching), false},
               {"scattered", &trees.images.at(scattered), false},
               {"side by side", &trees.images.at(side_by_side), true},
               {"two apart", &trees.images.at(two_apart), true},
               {"wide directory", &trees.images.at(wide), true},
               {"big", &trees.images.at(big), false},
               {"spaced", &trees.images.at(spaced), false},
               {"linked", &trees.images.at(linked), false}} {}

  std::vector<Scenario> Build() {
    for (const bool full : {false, true}) {
      AddDefaults(full);
      AddRunCounts(full);
      AddLooseRuns(full);
      AddKnownRecords(full);
      AddDirCounts(full);
    }
    AddMemory();
    for (const bool full : {false, true}) {
      AddFixtures(full);
    }
    for (const bool full : {false, true}) {
      AddDeletedRecover(full);
    }
    for (const bool full : {false, true}) {
      AddLaneOff(full);
    }
    return std::move(list_);
  }

 private:
  // Adds one scenario, named after its tree, its setting and its strategy. The
  // caller MAY set the media kind or the raw callback on the result.
  Scenario& Add(const std::string& name, const std::vector<BYTE>& image,
                bool full_cache, const VolumeOptions& options) {
    return list_.emplace_back(
        Scenario{.name = name + "/" + StrategyName(full_cache),
                 .image = &image,
                 .full_cache = full_cache,
                 .options = options});
  }

  // The tree at index.
  const NamedTree& Tree(TreeIndex index) const { return named_.at(index); }

  // The defaults, and the tight probes of the trees that need them.
  void AddDefaults(bool full) {
    for (const NamedTree& tree : named_) {
      Add(tree.name + "/default", *tree.image, full, Tuned({}));
      if (tree.tight) {
        Add(tree.name + "/default tight", *tree.image, full,
            Tuned({.tight = true}));
      }
    }
  }

  // How many runs the lane keeps: fewer than the plans hold, so that the stack
  // turns over, and more than there are runs at all.
  void AddRunCounts(bool full) {
    for (std::size_t index = 0; index <= wide; index++) {
      const NamedTree& tree = named_.at(index);
      for (const std::size_t runs : {1U, 2U, 3U, 4096U}) {
        Add(tree.name + "/runs=" + std::to_string(runs), *tree.image, full,
            Tuned({.runs = runs, .tight = tree.tight}));
      }
    }
  }

  // Records so close that a block holds both, with the default gap.
  void AddLooseRuns(bool full) {
    for (const TreeIndex index : {side_by_side, two_apart}) {
      Add(Tree(index).name + "/runs=3 loose", *Tree(index).image, full,
          Tuned({.runs = 3}));
    }
  }

  // A record named twice is probed once, until the memory of probed records is
  // full and starts again.
  void AddKnownRecords(bool full) {
    for (const std::uint64_t known : {4U, 64U}) {
      VolumeOptions options = Tuned({.runs = 3});
      options.tuning.probe.max_known_records = known;
      Add("linked/known=" + std::to_string(known), *Tree(linked).image, full,
          options);
      options.tuning.probe.max_chunk_records = 4;
      Add("linked/known=" + std::to_string(known) + " chunks=4",
          *Tree(linked).image, full, options);
    }
  }

  // How many directories are planned.
  void AddDirCounts(bool full) {
    for (const TreeIndex index : {grouped, branching, side_by_side}) {
      const NamedTree& tree = Tree(index);
      for (const std::size_t dirs : {1U, 4U}) {
        Add(tree.name + "/dirs=" + std::to_string(dirs), *tree.image, full,
            Tuned({.runs = 3, .dirs = dirs, .tight = tree.tight}));
      }
    }
  }

  // The record window and the memo, small or off. FullCache keeps neither.
  void AddMemory() {
    for (const TreeIndex index : {grouped, branching, side_by_side, wide}) {
      const NamedTree& tree = Tree(index);
      for (const Memory& memory :
           {Memory{"no memo", Knobs{}.window_bytes, 0},
            Memory{"no window no memo", 0, 0}, Memory{"small window", 8192, 16},
            Memory{"window 64 KiB", 65536, 16}}) {
        for (const std::size_t runs : {1U, 3U}) {
          Add(tree.name + "/" + memory.name + "/runs=" + std::to_string(runs),
              *tree.image, false,
              Tuned({.runs = runs,
                     .window_bytes = memory.window,
                     .memo_entries = memory.memo,
                     .tight = index == wide}));
        }
      }
    }
  }

  // Every tree of the other tests, under each way to open the volume.
  void AddFixtures(bool full) {
    for (const ReadAhead::TreeFixture& fixture : trees_.fixtures) {
      std::size_t mode = 0;
      for (const VolumeOptions& options : ReadAhead::AllOptions()) {
        Add(fixture.name + "/mode=" + std::to_string(mode++), fixture.image,
            full, options);
      }
    }
  }

  // Freed items and recovery, with a lane that holds few runs.
  void AddDeletedRecover(bool full) {
    VolumeOptions options = Tuned({.runs = 5, .dirs = 8});
    options.include_deleted = true;
    options.recover_errors = true;
    for (const TreeIndex index : {grouped, branching, scattered}) {
      Add(Tree(index).name + "/deleted recover", *Tree(index).image, full,
          options);
    }
  }

  // A raw attribute callback turns the lane off. So does a hard disk, unless
  // its own number of runs says otherwise.
  void AddLaneOff(bool full) {
    const std::vector<BYTE>& image = *Tree(grouped).image;
    Add("grouped/raw callback", image, full, Tuned({})).raw_callback = true;
    Add("grouped/rotational", image, full, Tuned({})).kind =
        Tuning::MediaKind::Rotational;
    VolumeOptions rotational = Tuned({});
    rotational.tuning.walk.read_ahead_runs_rotational = 8;
    Add("grouped/rotational runs=8", image, full, rotational).kind =
        Tuning::MediaKind::Rotational;
    Add("scattered/solid state", *Tree(scattered).image, full,
        Tuned({.runs = 8}))
        .kind = Tuning::MediaKind::SolidState;
  }

  const Trees& trees_;
  std::vector<NamedTree> named_;
  std::vector<Scenario> list_;
};

// The trees of the scenarios. Few and rich, each listed under the settings that
// change which lane calls are made. Each scenario name is unique.
const Trees& ScenarioTrees() {
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
  return trees;
}

// Every scenario.
const std::vector<Scenario>& Scenarios() {
  static const std::vector<Scenario> scenarios =
      ScenarioSet(ScenarioTrees()).Build();
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

// 1 when condition holds, else 0, to add to a counter.
std::size_t Count(bool condition) { return condition ? 1 : 0; }

// Whether the lane may run in the scenario: a raw callback and a hard disk
// turn it off.
bool LaneExpected(const Scenario& scenario) {
  return !scenario.raw_callback &&
         (scenario.kind != Tuning::MediaKind::Rotational ||
          scenario.options.tuning.walk.read_ahead_runs_rotational != 0);
}

// How many scenarios reach each path of the lane.
struct PathCounts {
  std::size_t with_lane{0};
  std::size_t withdrawing{0};
  std::size_t window_held{0};
  std::size_t unplanned{0};
  std::size_t kept_back{0};
  std::size_t longer_than_lane{0};
  std::size_t most_planned{0};

  void Add(const Scenario& scenario, const Outcome& outcome) {
    const Io::DirectoryReadAheadStats& stats = outcome.stats;
    with_lane += Count(!outcome.lane.empty());
    withdrawing += Count(stats.runs_withdrawn != 0);
    window_held += Count(stats.runs_window_held != 0);
    unplanned += Count(stats.runs_unplanned != 0);
    // Planned runs that the lane never announced: the window held them, or an
    // overlap kept them off the lane until the other run was read.
    kept_back +=
        Count(scenario.full_cache && stats.runs_announced < stats.runs_planned);
    longer_than_lane += Count(stats.runs_planned >
                              2 * scenario.options.tuning.walk.read_ahead_runs);
    most_planned =
        (std::max)(most_planned, static_cast<std::size_t>(stats.runs_planned));
  }
};

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
  PathCounts counts;
  for (std::size_t index = 0; index < Scenarios().size(); index++) {
    const Scenario& scenario = Scenarios().at(index);
    const Outcome& outcome = Outcomes().at(index);
    INFO(scenario.name);
    if (!LaneExpected(scenario)) {
      CHECK(outcome.lane.empty());
      CHECK(outcome.stats.runs_planned == 0);
    }
    counts.Add(scenario, outcome);
  }
  CHECK(counts.with_lane > 100);
  CHECK(counts.withdrawing > 20);
  CHECK(counts.window_held > 0);
  CHECK(counts.unplanned > 10);
  CHECK(counts.kept_back > 0);
  CHECK(counts.longer_than_lane > 30);
  CHECK(counts.most_planned >= 100);
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
