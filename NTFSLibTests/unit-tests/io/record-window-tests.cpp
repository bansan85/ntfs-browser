#include <cstddef>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "io/mft-runs.h"
#include "io/record-window.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;

namespace {

// A record of 8 bytes keeps the cases short.
constexpr std::size_t record_size = 8;

// count records, each filled with its own slot number plus first.
std::vector<BYTE> Records(std::size_t count, BYTE first) {
  std::vector<BYTE> bytes;
  for (std::size_t index = 0; index < count; index++) {
    bytes.insert(bytes.end(), record_size, static_cast<BYTE>(first + index));
  }
  return bytes;
}

}  // namespace

TEST_CASE("A record window gives back the records it was given",
          "[io][record-window]") {
  Io::RecordWindow window(1024, record_size);
  CHECK(window.Enabled());
  CHECK(window.Find(10).empty());

  const std::vector<BYTE> run = Records(3, 1);
  window.Put(10, run);
  CHECK(window.HeldBytes() == run.size());
  for (ULONGLONG slot = 10; slot < 13; slot++) {
    const auto found = window.Find(slot);
    REQUIRE(found.size() == record_size);
    CHECK(found.front() == static_cast<BYTE>(1 + slot - 10));
    CHECK(found.back() == found.front());
  }
  CHECK(window.Find(9).empty());
  CHECK(window.Find(13).empty());
}

TEST_CASE("A record window of 0 holds nothing", "[io][record-window]") {
  Io::RecordWindow window(0, record_size);
  CHECK_FALSE(window.Enabled());
  window.Put(5, Records(1, 1));
  CHECK(window.HeldBytes() == 0);
  CHECK(window.Find(5).empty());
}

TEST_CASE("A record window drops the oldest run when it is full",
          "[io][record-window]") {
  // Room for two runs of two records.
  Io::RecordWindow window(4 * record_size, record_size);
  window.Put(0, Records(2, 10));
  window.Put(100, Records(2, 20));
  CHECK(window.HeldBytes() == 4 * record_size);
  CHECK_FALSE(window.Find(0).empty());

  window.Put(200, Records(2, 30));
  CHECK(window.HeldBytes() == 4 * record_size);
  CHECK(window.Find(0).empty());
  CHECK(window.Find(1).empty());
  CHECK_FALSE(window.Find(100).empty());
  CHECK(window.Find(200).front() == 30);
}

TEST_CASE("A record window keeps the newest copy of a record",
          "[io][record-window]") {
  Io::RecordWindow window(4 * record_size, record_size);
  window.Put(0, Records(2, 10));
  // Slot 1 is read again, by a run that holds it in its middle.
  window.Put(1, Records(2, 50));
  CHECK(window.Find(1).front() == 50);
  CHECK(window.Find(2).front() == 51);

  // The first run goes: slot 0 goes with it, slot 1 stays with the second.
  window.Put(300, Records(2, 70));
  CHECK(window.Find(0).empty());
  REQUIRE_FALSE(window.Find(1).empty());
  CHECK(window.Find(1).front() == 50);
  CHECK(window.Find(300).front() == 70);
}

TEST_CASE("A record window holds no run that alone outgrows it",
          "[io][record-window]") {
  Io::RecordWindow window(2 * record_size, record_size);
  window.Put(0, Records(1, 1));
  window.Put(10, Records(3, 5));
  CHECK(window.Find(10).empty());
  CHECK_FALSE(window.Find(0).empty());
  CHECK(window.HeldBytes() == record_size);
}

TEST_CASE("A record window ignores the partial record that ends a run",
          "[io][record-window]") {
  Io::RecordWindow window(1024, record_size);
  std::vector<BYTE> bytes = Records(2, 1);
  bytes.insert(bytes.end(), record_size - 1, BYTE{9});
  window.Put(0, bytes);
  CHECK(window.HeldBytes() == 2 * record_size);
  CHECK(window.Find(2).empty());
}

TEST_CASE("Runs of $MFT widen to the clusters that hold them",
          "[io][record-window]") {
  const std::vector<ULONGLONG> slots{17, 18, 40, 41, 42, 100};
  const std::vector<Io::MftRun> planned = Io::PlanMftRuns(slots, 2, 100);
  REQUIRE(planned.size() == 3);
  CHECK(planned.at(0).first_slot == 17);
  CHECK(planned.at(0).last_slot == 18);

  // Four records a cluster.
  const std::vector<Io::MftRun> widened = Io::WidenToClusters(planned, 4, 16);
  REQUIRE(widened.size() == 3);
  CHECK(widened.at(0).first_slot == 16);
  CHECK(widened.at(0).last_slot == 19);
  CHECK(widened.at(1).first_slot == 40);
  CHECK(widened.at(1).last_slot == 43);
  CHECK(widened.at(2).first_slot == 100);
  CHECK(widened.at(2).last_slot == 103);
  // Each still serves the slots it was planned for.
  CHECK(widened.at(0).begin == 0);
  CHECK(widened.at(2).end == slots.size());
}

TEST_CASE("Runs that share a cluster once widened are one run",
          "[io][record-window]") {
  const std::vector<ULONGLONG> slots{5, 7, 8, 30};
  // A hole of one slot splits 5 from 7 and 8, and 30 is far.
  const std::vector<Io::MftRun> planned = Io::PlanMftRuns(slots, 0, 100);
  REQUIRE(planned.size() == 3);

  const std::vector<Io::MftRun> widened = Io::WidenToClusters(planned, 4, 0);
  REQUIRE(widened.size() == 2);
  // 5 is in the cluster of slots 4 to 7, and 7 and 8 are in it and the next.
  CHECK(widened.at(0).first_slot == 4);
  CHECK(widened.at(0).last_slot == 11);
  CHECK(widened.at(0).begin == 0);
  CHECK(widened.at(0).end == 3);
  CHECK(widened.at(1).first_slot == 28);
  CHECK(widened.at(1).last_slot == 31);
}

TEST_CASE("Runs are left alone when a cluster holds a record or less",
          "[io][record-window]") {
  const std::vector<ULONGLONG> slots{17, 18, 40};
  const std::vector<Io::MftRun> planned = Io::PlanMftRuns(slots, 2, 100);
  for (const ULONGLONG per_cluster : {ULONGLONG{0}, ULONGLONG{1}}) {
    const std::vector<Io::MftRun> same =
        Io::WidenToClusters(planned, per_cluster, 16);
    REQUIRE(same.size() == planned.size());
    for (std::size_t index = 0; index < same.size(); index++) {
      CHECK(same.at(index).first_slot == planned.at(index).first_slot);
      CHECK(same.at(index).last_slot == planned.at(index).last_slot);
    }
  }
}

TEST_CASE("Runs of $MFT do not widen below the first readable slot",
          "[io][record-window]") {
  const std::vector<ULONGLONG> slots{17, 18, 200};
  const std::vector<Io::MftRun> planned = Io::PlanMftRuns(slots, 2, 100);
  REQUIRE(planned.size() == 2);

  // Sixty-four records a cluster: the first cluster holds slots 0 to 63.
  const std::vector<Io::MftRun> widened = Io::WidenToClusters(planned, 64, 16);
  REQUIRE(widened.size() == 2);
  CHECK(widened.at(0).first_slot == 16);
  CHECK(widened.at(0).last_slot == 63);
  CHECK(widened.at(1).first_slot == 192);
  CHECK(widened.at(1).last_slot == 255);
}

TEST_CASE("A wide hole is read across only by a run that stays dense",
          "[io][mft-runs]") {
  // Slots 0 to 9 are wanted, then 300 and 310: 290 free slots of hole.
  std::vector<ULONGLONG> slots;
  for (ULONGLONG slot = 0; slot < 10; slot++) {
    slots.push_back(slot);
  }
  slots.push_back(300);
  slots.push_back(310);

  // Narrow holes of 2 slots: the far records are alone.
  CHECK(Io::PlanMftRuns(slots, 2, 1000).size() == 3);
  // The hole of 290 slots fits the wide gap, but the 11 slots wanted of 301
  // are 36 per thousand: under 125. 300 and 310 are two of 11: 181.
  const Io::WideGap eighth{.gap_slots = 400, .min_share_per_mille = 125};
  const std::vector<Io::MftRun> apart = Io::PlanMftRuns(slots, 2, 1000, eighth);
  REQUIRE(apart.size() == 2);
  CHECK(apart.at(0).last_slot == 9);
  CHECK(apart.at(1).first_slot == 300);
  // At 30 per thousand the run is dense enough, and takes 300 and 310 too.
  const Io::WideGap thin{.gap_slots = 400, .min_share_per_mille = 30};
  const std::vector<Io::MftRun> joined = Io::PlanMftRuns(slots, 2, 1000, thin);
  REQUIRE(joined.size() == 1);
  CHECK(joined.at(0).first_slot == 0);
  CHECK(joined.at(0).last_slot == 310);
  CHECK(joined.at(0).end == slots.size());
}

TEST_CASE("A wide hole crossed stays within the dense share and the run cap",
          "[io][mft-runs]") {
  // Every fourth slot: 250 per thousand, holes of 3.
  std::vector<ULONGLONG> slots;
  for (ULONGLONG slot = 0; slot < 400; slot += 4) {
    slots.push_back(slot);
  }
  // A hole of 3 is wider than 2: only the wide gap joins the records.
  CHECK(Io::PlanMftRuns(slots, 2, 1000).size() == slots.size());
  const Io::WideGap quarter{.gap_slots = 8, .min_share_per_mille = 200};
  CHECK(Io::PlanMftRuns(slots, 2, 1000, quarter).size() == 1);
  // A demand above the density of the slots cuts the run where the start has
  // stopped paying for the rest.
  const Io::WideGap more{.gap_slots = 8, .min_share_per_mille = 300};
  const size_t cut = Io::PlanMftRuns(slots, 2, 1000, more).size();
  CHECK(cut > 1);
  CHECK(cut < slots.size());
  // The cap on the run still cuts it: 100 slots span at most.
  const std::vector<Io::MftRun> capped =
      Io::PlanMftRuns(slots, 2, 100, quarter);
  REQUIRE(capped.size() == 4);
  for (const Io::MftRun& run : capped) {
    CHECK(run.last_slot - run.first_slot + 1 <= 100);
  }
}

TEST_CASE("Only a hard disk read through a cache has a wide gap",
          "[io][mft-runs]") {
  const Tuning::ProbeTuning probe;
  const ULONGLONG record = 1024;

  const Io::WideGap hard =
      Io::WideGapOf(probe, Tuning::MediaKind::Rotational, record, true);
  CHECK(hard.gap_slots == probe.wide_gap_bytes / record);
  CHECK(hard.min_share_per_mille == probe.wide_gap_min_share_per_mille);
  for (const Tuning::MediaKind kind :
       {Tuning::MediaKind::Unknown, Tuning::MediaKind::SolidState}) {
    CHECK(Io::WideGapOf(probe, kind, record, true).gap_slots == 0);
  }
  // NoCache keeps nothing, and would read a big directory's runs again.
  CHECK(Io::WideGapOf(probe, Tuning::MediaKind::Rotational, record, false)
            .gap_slots == 0);
  // A wide gap that is not wider than the narrow one is no gap.
  Tuning::ProbeTuning narrow = probe;
  narrow.wide_gap_bytes = narrow.max_gap_bytes;
  CHECK(Io::WideGapOf(narrow, Tuning::MediaKind::Rotational, record, true)
            .gap_slots == 0);
  CHECK(
      Io::WideGapOf(probe, Tuning::MediaKind::Rotational, 0, true).gap_slots ==
      0);
}
