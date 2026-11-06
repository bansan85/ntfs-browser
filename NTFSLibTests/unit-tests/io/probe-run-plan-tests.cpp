#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "io/mft-runs.h"
#include "probe-walk-support.h"

namespace NtfsBrowser {

class IndexEntryView;

}  // namespace NtfsBrowser

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
namespace ProbeWalk = NTFS_BROWSER_NAMESPACE::Tests::ProbeWalk;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using Tests::CountingDiskReader;
using Tests::FakeTreeNode;

namespace {

// A run, as (first slot, how many slots).
using Run = std::pair<ULONGLONG, ULONGLONG>;

// Slots of the files of the directory of RunTree(). The holes between them,
// in free records of $MFT, are 3, 100 and 1200 slots wide: a narrow hole
// (64 KiB is 64 slots of 1 KiB) joins its neighbours, the second only on a
// hard disk that caches, the third never (the wide gap is 1 MiB, 1024 slots).
constexpr ULONGLONG slot_a = 17;
constexpr ULONGLONG slot_b = 18;
constexpr ULONGLONG slot_c = 22;
constexpr ULONGLONG slot_d = 23;
constexpr ULONGLONG slot_e = 124;
constexpr ULONGLONG slot_f = 1325;

// The slots of the files, in index order.
const std::vector<ULONGLONG>& FileSlots() {
  static const std::vector<ULONGLONG> slots{slot_a, slot_b, slot_c,
                                            slot_d, slot_e, slot_f};
  return slots;
}

// A node that no index names: a freed record with a name of its own.
FakeTreeNode Free() {
  return {.links = {{.name = L"x", .parent = Tests::name_tree_root}},
          .in_use = false};
}

// A file of the directory, node 0.
FakeTreeNode File(wchar_t name) {
  return {.links = {{.name = std::wstring(1, name), .parent = 0}}};
}

// A directory (node 0, record 16) whose six files sit at FileSlots(), with
// freed records between them. Its index fits its record: one node, one probe
// group, so one chunk.
std::vector<FakeTreeNode> RunTree() {
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"dir", .parent = Tests::name_tree_root}},
                   .directory = true});
  const auto free_until = [&nodes](ULONGLONG slot) {
    while (Tests::name_tree_first_record + nodes.size() < slot) {
      nodes.push_back(Free());
    }
  };
  wchar_t name = L'a';
  for (const ULONGLONG slot : FileSlots()) {
    free_until(slot);
    nodes.push_back(File(name++));
  }
  return nodes;
}

// The byte ranges a walk announced to the reader, as (offset, length).
std::vector<Run> Announced(const std::vector<CountingDiskReader::Call>& calls) {
  std::vector<Run> ranges;
  for (const CountingDiskReader::Call& call : calls) {
    if (call.kind == CountingDiskReader::Kind::Prefetch) {
      ranges.emplace_back(call.offset, call.length);
    }
  }
  return ranges;
}

// The runs of a plan, as (first slot, how many slots).
std::vector<Run> RunsOf(const Io::ProbeRunPlan& plan) {
  std::vector<Run> runs;
  for (const Io::MftRun& run : plan.runs) {
    runs.emplace_back(run.first_slot, run.last_slot - run.first_slot + 1);
  }
  return runs;
}

// The slots of rules that a test of the pure planner uses: records of 1 KiB, a
// narrow hole of 64 slots, runs of 1024 at most, nothing else.
Io::ProbeRunRules PlainRules() {
  return {.mapped = true,
          .record_size = Tests::fake_file_record_size,
          .max_gap_slots = 64,
          .max_run_slots = 1024,
          .wide = {},
          .slots_per_cluster = 0,
          .first_mapped_slot = 16};
}

// What a run of a given kind of device is, for a strategy.
struct ProbeVariant {
  Tuning::MediaKind kind;
  bool keeps;
  bool holds_e;
};

// The first slot a cache is asked for. The blocks that hold the directory's
// own record are in the cache already, and are neither announced nor read
// again.
constexpr ULONGLONG cached_until = 24;

// The slots up to this one are left out of what a strategy is asked for.
template <Cache::Strategy S>
ULONGLONG LeftOut() {
  return S == Cache::Strategy::FullCache ? cached_until : 0;
}

// The byte ranges that runs cover from left_out on.
std::vector<Run> WantedRanges(const std::vector<Run>& runs, ULONGLONG mft,
                              ULONGLONG record_size, ULONGLONG left_out) {
  std::vector<Run> wanted;
  for (const auto& [first, count] : runs) {
    const ULONGLONG from = (std::max)(first, left_out);
    if (from < first + count) {
      wanted.emplace_back(mft + (from * record_size),
                          (first + count - from) * record_size);
    }
  }
  return wanted;
}

// The bytes of the record at slot_e as the disk has them, when the store holds
// it.
std::map<ULONGLONG, std::vector<BYTE>>
    HeldRecordsOf(const std::vector<BYTE>& image, ULONGLONG mft,
                  ULONGLONG record_size, bool holds_e) {
  std::map<ULONGLONG, std::vector<BYTE>> held_records;
  if (holds_e) {
    const auto first = image.begin() + static_cast<std::ptrdiff_t>(
                                           mft + (slot_e * record_size));
    held_records[slot_e].assign(
        first, first + static_cast<std::ptrdiff_t>(record_size));
  }
  return held_records;
}

// A cache announces the blocks that hold the runs, so a range only has to be
// inside what it announced. Without a cache the ranges are the runs.
template <Cache::Strategy S>
void CheckAnnounced(const std::vector<Run>& announced,
                    const std::vector<Run>& wanted) {
  if constexpr (S == Cache::Strategy::NoCache) {
    CHECK(announced == wanted);
  } else {
    for (const auto& [offset, length] : wanted) {
      CAPTURE(offset, length);
      const bool covered =
          std::ranges::any_of(announced, [offset, length](const Run& hint) {
            return hint.first <= offset &&
                   hint.first + hint.second >= offset + length;
          });
      CHECK(covered);
    }
  }
}

// Walks the directory of RunTree() over a volume of the variant, and checks
// that the probes read the runs that PlanProbeRuns plans.
template <Cache::Strategy S>
void CheckProbeRuns(const std::vector<BYTE>& image, const ProbeVariant& variant,
                    const std::vector<Run>& expected_runs) {
  constexpr ULONGLONG directory = Tests::name_tree_first_record;
  auto open = ProbeWalk::Open<S>(image, variant.kind);
  REQUIRE(ProbeWalk::BlocksOf(*open.volume, directory).empty());
  const ULONGLONG mft = open.volume->GetMFTAddr();
  const ULONGLONG record_size = open.volume->GetFileRecordSize();
  const auto held_records =
      HeldRecordsOf(image, mft, record_size, variant.holds_e);

  const ProbeWalk::DirectoryWalk walk = ProbeWalk::WalkDirectory<S>(
      open, directory, {.held_records = &held_records, .keeps = variant.keeps});
  REQUIRE(walk.groups.size() == 1);
  CHECK(walk.listed.size() == FileSlots().size());

  // One chunk: the records came in one ascending run, each asked once.
  CHECK(walk.asked == FileSlots());

  // The runs of the plan are the runs the probe read, and noted, in order.
  const Io::ProbeRunRules rules =
      Io::ProbeRunRulesOf(*open.volume, variant.keeps);
  const Io::ProbeRunPlan plan =
      Io::PlanProbeRuns(FileSlots(), rules,
                        [&held_records](ULONGLONG slot) {
                          const auto bytes = held_records.find(slot);
                          return bytes == held_records.end()
                                     ? std::span<const BYTE>()
                                     : std::span<const BYTE>(bytes->second);
                        },
                        {});
  CHECK(RunsOf(plan) == expected_runs);
  CHECK(walk.ran == expected_runs);

  // They were announced as they were planned.
  CheckAnnounced<S>(
      Announced(walk.calls),
      WantedRanges(expected_runs, mft, record_size, LeftOut<S>()));

  // Every record of every run was read, but the one the store held.
  for (const ULONGLONG slot : FileSlots()) {
    const ULONGLONG offset = mft + (slot * record_size);
    const bool read =
        open.reads->ReadsInRange(offset, offset + record_size) != 0;
    CAPTURE(slot);
    if (slot >= LeftOut<S>()) {
      CHECK(read == !(variant.holds_e && slot == slot_e));
    }
  }
}

}  // namespace

TEST_CASE("The plan of a chunk reads the records no one holds in runs",
          "[io][probe-run-plan]") {
  const std::vector<ULONGLONG> chunk{5,      slot_a, slot_b, slot_c,
                                     slot_d, slot_e, slot_f};
  Io::ProbeRunRules rules = PlainRules();

  SECTION("narrow holes only") {
    std::vector<ULONGLONG> asked;
    const Io::ProbeRunPlan plan =
        Io::PlanProbeRuns(chunk, rules,
                          [&asked](ULONGLONG record) {
                            asked.push_back(record);
                            return std::span<const BYTE>();
                          },
                          {});
    // Record 5 has a fixed address: it is probed alone, and never asked for.
    CHECK(plan.alone == 1);
    CHECK(asked == FileSlots());
    CHECK(plan.to_read == FileSlots());
    CHECK(RunsOf(plan) == std::vector<Run>{{17, 7}, {124, 1}, {1325, 1}});
    // A run serves its records as indices of to_read.
    REQUIRE(plan.runs.size() == 3);
    CHECK(plan.runs.at(0).begin == 0);
    CHECK(plan.runs.at(0).end == 4);
    CHECK(plan.runs.at(2).begin == 5);
    CHECK(plan.runs.at(2).end == 6);
  }

  SECTION("a hard disk crosses the wide hole") {
    rules.wide = {.gap_slots = 1024, .min_share_per_mille = 10};
    const Io::ProbeRunPlan plan = Io::PlanProbeRuns(chunk, rules, {}, {});
    CHECK(RunsOf(plan) == std::vector<Run>{{17, 108}, {1325, 1}});
  }

  SECTION("a record the store holds is not read") {
    const std::vector<BYTE> bytes(Tests::fake_file_record_size);
    std::vector<ULONGLONG> held;
    const Io::ProbeRunPlan plan = Io::PlanProbeRuns(
        chunk, rules,
        [&bytes](ULONGLONG record) {
          return record == slot_e ? std::span<const BYTE>(bytes)
                                  : std::span<const BYTE>();
        },
        [&held](ULONGLONG record, std::span<const BYTE> held_bytes) {
          CHECK(held_bytes.size() == Tests::fake_file_record_size);
          held.push_back(record);
        });
    CHECK(held == std::vector<ULONGLONG>{slot_e});
    CHECK(plan.to_read ==
          std::vector<ULONGLONG>{slot_a, slot_b, slot_c, slot_d, slot_f});
    CHECK(RunsOf(plan) == std::vector<Run>{{17, 7}, {1325, 1}});
  }

  SECTION("bytes of another size are not a held record") {
    const std::vector<BYTE> shorter(Tests::fake_file_record_size - 1);
    const Io::ProbeRunPlan plan = Io::PlanProbeRuns(
        chunk, rules,
        [&shorter](ULONGLONG) { return std::span<const BYTE>(shorter); }, {});
    CHECK(plan.to_read == FileSlots());
  }

  SECTION("a kept run is widened to whole clusters") {
    rules.slots_per_cluster = 4;
    const Io::ProbeRunPlan plan = Io::PlanProbeRuns(chunk, rules, {}, {});
    CHECK(RunsOf(plan) == std::vector<Run>{{16, 8}, {124, 4}, {1324, 4}});
  }

  SECTION("an $MFT that is not mapped is probed record by record") {
    rules.mapped = false;
    bool asked = false;
    const Io::ProbeRunPlan plan =
        Io::PlanProbeRuns(chunk, rules,
                          [&asked](ULONGLONG) {
                            asked = true;
                            return std::span<const BYTE>();
                          },
                          {});
    CHECK(plan.alone == chunk.size());
    CHECK(plan.to_read.empty());
    CHECK(plan.runs.empty());
    CHECK_FALSE(asked);
  }

  SECTION("an empty chunk plans nothing") {
    const Io::ProbeRunPlan plan = Io::PlanProbeRuns({}, rules, {}, {});
    CHECK(plan.alone == 0);
    CHECK(plan.runs.empty());
  }
}

TEMPLATE_TEST_CASE_SIG("A probe reads the runs that PlanProbeRuns plans",
                       "[io][probe-run-plan]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithNameTree(RunTree());
  const auto check = [&image](const ProbeVariant& variant,
                              const std::vector<Run>& expected_runs) {
    CheckProbeRuns<S>(image, variant, expected_runs);
  };

  SECTION("an unknown device, default gaps") {
    const std::vector<Run> expected{{17, 7}, {124, 1}, {1325, 1}};
    check({Tuning::MediaKind::Unknown, false, false}, expected);
    check({Tuning::MediaKind::SolidState, false, false}, expected);
  }

  SECTION("a hard disk crosses the wide hole only when the volume caches") {
    if constexpr (S == Cache::Strategy::FullCache) {
      check({Tuning::MediaKind::Rotational, false, false},
            {{17, 108}, {1325, 1}});
    } else {
      check({Tuning::MediaKind::Rotational, false, false},
            {{17, 7}, {124, 1}, {1325, 1}});
    }
  }

  SECTION("a record that the store holds leaves the runs") {
    check({Tuning::MediaKind::Unknown, false, true}, {{17, 7}, {1325, 1}});
    if constexpr (S == Cache::Strategy::FullCache) {
      check({Tuning::MediaKind::Rotational, false, true}, {{17, 7}, {1325, 1}});
    }
  }

  SECTION("a store that keeps the runs") {
    check({Tuning::MediaKind::Unknown, true, false},
          {{17, 7}, {124, 1}, {1325, 1}});
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A store that only notes the runs is called after each "
    "run, in order",
    "[io][probe-run-plan]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithNameTree(RunTree());
  constexpr ULONGLONG directory = Tests::name_tree_first_record;

  auto plain = ProbeWalk::Open<S>(image);
  const ProbeWalk::DirectoryWalk reference =
      ProbeWalk::WalkDirectory<S>(plain, directory, {});

  auto open = ProbeWalk::Open<S>(image);
  const ProbeWalk::DirectoryWalk walk =
      ProbeWalk::WalkDirectory<S>(open, directory, {.note_only = true});
  CHECK(walk.ran == std::vector<Run>{{17, 7}, {124, 1}, {1325, 1}});
  // No find and no keep: the walk lists, announces and reads as it did.
  CHECK(walk.listed == reference.listed);
  REQUIRE(walk.calls.size() == reference.calls.size());
  for (std::size_t index = 0; index < walk.calls.size(); index++) {
    CHECK(walk.calls.at(index).kind == reference.calls.at(index).kind);
    CHECK(walk.calls.at(index).offset == reference.calls.at(index).offset);
    CHECK(walk.calls.at(index).length == reference.calls.at(index).length);
  }
}

TEMPLATE_TEST_CASE_SIG("The plain walk never calls the note of a probe store",
                       "[io][probe-run-plan]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithNameTree(RunTree());
  auto open = ProbeWalk::Open<S>(image);
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(*open.volume);
  record.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                     NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(record.ParseFileRecord(Tests::name_tree_first_record));
  REQUIRE(record.ParseAttrs());

  std::size_t noted = 0;
  record.SetProbeStore({.find = {},
                        .keep = {},
                        .ran = [&noted](ULONGLONG, ULONGLONG) { noted++; }});
  std::size_t listed = 0;
  record.TraverseSubEntries(
      [](const IndexEntryView&, void* context) {
        (*static_cast<std::size_t*>(context))++;
      },
      &listed);
  CHECK(listed == FileSlots().size());
  CHECK(noted == 0);
}
