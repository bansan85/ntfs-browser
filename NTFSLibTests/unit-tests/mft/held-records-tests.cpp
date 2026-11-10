#include <cstddef>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "mft/held-records.h"
#include "mft/idx.h"
#include "win-types.h"

namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

// Bytes of one record, and of the block and the budget of the tests: a block
// holds four records, and the budget three.
constexpr std::size_t record_size = 64;
constexpr std::size_t block_bytes = 4 * record_size;
constexpr std::size_t budget_bytes = 3 * record_size;

// A record of record_size bytes, all of them fill.
std::vector<BYTE> RecordOf(BYTE fill) {
  return std::vector<BYTE>(record_size, fill);
}

// A waiting base record number record with no bytes held and no extension.
Mft::HeldRecords::Waiting WaitingRecord(ULONGLONG record) {
  return {.record = record, .entry_index = 0};
}

}  // namespace

TEST_CASE("the base of a slot is remembered from the first user slot on",
          "[mft][held-records]") {
  Mft::SlotBases bases(100);
  const auto first_user = static_cast<ULONGLONG>(Mft::Idx::User);

  bases.Note(first_user - 1, 7);
  bases.Note(first_user, 0);
  bases.Note(first_user + 4, 20);
  bases.Note(first_user + 5, ULONGLONG{1} << 40);
  bases.Note(Mft::max_remembered_slots, 3);

  CHECK_FALSE(bases.BaseOf(first_user - 1));
  CHECK(bases.BaseOf(first_user) == std::optional<ULONGLONG>(0));
  CHECK(bases.BaseOf(first_user + 4) == std::optional<ULONGLONG>(20));
  // A base too large for four bytes is not known.
  CHECK_FALSE(bases.BaseOf(first_user + 5));
  // A slot nobody noted.
  CHECK_FALSE(bases.BaseOf(first_user + 1));
  CHECK_FALSE(bases.BaseOf(Mft::max_remembered_slots));
}

TEST_CASE("an extension record that names itself is not held",
          "[mft][held-records]") {
  Mft::HeldRecords held(record_size, block_bytes, budget_bytes);

  held.MeetExtension(40, 40, RecordOf(1));
  held.Confirm(40);

  CHECK(held.FindExtension(40).empty());
}

TEST_CASE(
    "an extension record met before its base is held when the base "
    "is confirmed",
    "[mft][held-records]") {
  Mft::HeldRecords held(record_size, block_bytes, budget_bytes);

  held.MeetExtension(30, 40, RecordOf(0x11));
  CHECK(held.FindExtension(30).empty());
  held.Confirm(40);

  const auto bytes = held.FindExtension(30);
  REQUIRE(bytes.size() == record_size);
  CHECK(bytes.front() == 0x11);
}

TEST_CASE("an extension record that the budget cannot hold is not confirmed",
          "[mft][held-records]") {
  Mft::HeldRecords held(record_size, block_bytes, 0);

  held.MeetExtension(30, 40, RecordOf(0x11));
  held.Confirm(40);

  CHECK(held.FindExtension(30).empty());
}

TEST_CASE("an extension record whose base does not wait gives its slot back",
          "[mft][held-records]") {
  Mft::HeldRecords held(record_size, block_bytes, record_size);

  SECTION("released before a later record") {
    held.MeetExtension(30, 40, RecordOf(0x11));
    held.ReleaseBefore(41);
    held.Confirm(40);
    CHECK(held.FindExtension(30).empty());
  }

  SECTION("released at the end of the pass") {
    held.MeetExtension(30, 40, RecordOf(0x11));
    held.Finish();
    held.Confirm(40);
    CHECK(held.FindExtension(30).empty());
  }

  SECTION("the slot serves the next extension record") {
    held.MeetExtension(30, 40, RecordOf(0x11));
    held.Finish();

    held.MeetExtension(31, 32, RecordOf(0x22));
    held.Confirm(32);

    const auto bytes = held.FindExtension(31);
    REQUIRE(bytes.size() == record_size);
    CHECK(bytes.front() == 0x22);
  }
}

TEST_CASE("a waiting base record collects its extension records",
          "[mft][held-records]") {
  Mft::HeldRecords held(record_size, block_bytes, budget_bytes);

  SECTION("a record that is not a whole record does not wait") {
    CHECK_FALSE(held.HoldWaiting(WaitingRecord(20), std::vector<BYTE>(3)));
  }

  SECTION("an extension met before it is held for it") {
    held.MeetExtension(18, 20, RecordOf(0x33));

    REQUIRE(held.HoldWaiting(WaitingRecord(20), RecordOf(0x44)));
    held.MeetExtension(22, 20, RecordOf(0x55));

    REQUIRE(held.WaitingRecords().size() == 1);
    CHECK(held.WaitingRecords().front().record == 20);
    CHECK(held.WaitingRecords().front().slot.has_value());
    CHECK(held.WaitingRecords().front().unheld_extensions.empty());
    CHECK(held.FindExtension(18).front() == 0x33);
    CHECK(held.FindExtension(22).front() == 0x55);
  }

  SECTION("an extension met after a base that is not waiting is ignored") {
    held.MeetExtension(22, 21, RecordOf(0x55));

    CHECK(held.FindExtension(22).empty());
  }
}

TEST_CASE("the budget leaves a record to be read again",
          "[mft][held-records]") {
  Mft::HeldRecords held(record_size, block_bytes, record_size);

  REQUIRE(held.HoldWaiting(WaitingRecord(20), RecordOf(0x44)));
  held.MeetExtension(22, 20, RecordOf(0x55));
  REQUIRE(held.HoldWaiting(WaitingRecord(30), RecordOf(0x66)));

  REQUIRE(held.WaitingRecords().size() == 2);
  // The first base took the one slot of the budget.
  CHECK(held.WaitingRecords().at(0).slot.has_value());
  CHECK(held.WaitingRecords().at(0).unheld_extensions ==
        std::vector<ULONGLONG>{22});
  CHECK_FALSE(held.WaitingRecords().at(1).slot.has_value());
  CHECK(held.FindExtension(22).empty());
}
