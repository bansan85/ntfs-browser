#include <cstddef>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "mft/entry-table.h"
#include "win-types.h"

namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

// A record number far past what the dense index of a small table covers.
constexpr ULONGLONG far_record = ULONGLONG{1} << 40;

// Appends a kept entry for record to table.
void Keep(Mft::EntryTable& table, ULONGLONG record) {
  Mft::MftTree::Entry entry;
  entry.record = record;
  table.Entries().push_back(entry);
}

}  // namespace

TEST_CASE("an entry table finds a record by its number", "[mft][entry-table]") {
  Mft::EntryTable table;
  Keep(table, 5);
  Keep(table, 16);
  Keep(table, far_record);
  table.IndexAll();

  SECTION("a record in the dense index") {
    CHECK(table.IndexOf(16) == std::optional<size_t>(1));
    REQUIRE(table.Find(5) != nullptr);
    CHECK(table.Find(5)->record == 5);
  }

  SECTION("a record far past the dense index") {
    CHECK(table.IndexOf(far_record) == std::optional<size_t>(2));
    REQUIRE(table.Find(far_record) != nullptr);
    CHECK(table.Find(far_record)->record == far_record);
  }

  SECTION("a record the scan did not keep") {
    CHECK_FALSE(table.IndexOf(6));
    CHECK_FALSE(table.IndexOf(far_record + 1));
    CHECK(table.Find(far_record + 1) == nullptr);
    CHECK(table.Find(1000) == nullptr);
  }
}

TEST_CASE("an entry table finds nothing before IndexAll",
          "[mft][entry-table]") {
  Mft::EntryTable table;
  Keep(table, 5);

  CHECK_FALSE(table.IndexOf(5));
  CHECK(table.Find(5) == nullptr);
}
