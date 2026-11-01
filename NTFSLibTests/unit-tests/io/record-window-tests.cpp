#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "io/mft-runs.h"
#include "io/record-window.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;

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
