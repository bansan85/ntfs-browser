#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

namespace Lib = NTFS_BROWSER_NAMESPACE::Search;

TEST_CASE("A Date converts to and from Unix seconds", "[search][types]") {
  constexpr Lib::Date epoch = Lib::Date::FromUnixSeconds(0);

  CHECK(epoch.ticks == 116'444'736'000'000'000);
  CHECK(epoch.ToUnixSeconds() == 0);
  CHECK(Lib::Date::FromUnixSeconds(1'700'000'000).ToUnixSeconds() ==
        1'700'000'000);
  CHECK(Lib::Date::FromUnixSeconds(-1).ToUnixSeconds() == -1);
}

TEST_CASE("A Date before 1601 is the unset date", "[search][types]") {
  CHECK_FALSE(Lib::Date::FromUnixSeconds(-12'000'000'000).IsSet());
  CHECK_FALSE(Lib::Date{}.IsSet());
  CHECK(Lib::Date{.ticks = 1}.IsSet());
}

TEST_CASE("Dates order by their ticks", "[search][types]") {
  CHECK(Lib::Date{.ticks = 1} < Lib::Date{.ticks = 2});
  CHECK(Lib::Date{.ticks = 2} == Lib::Date{.ticks = 2});
}

TEST_CASE("An entry knows when it names a deleted file", "[search][types]") {
  Lib::Entry entry;
  CHECK_FALSE(entry.IsDeleted());
  entry.standing = Lib::Standing::Deleted;
  CHECK(entry.IsDeleted());
  entry.standing = Lib::Standing::Reused;
  CHECK(entry.IsDeleted());
  entry.standing = Lib::Standing::Unreadable;
  CHECK_FALSE(entry.IsDeleted());
}

TEST_CASE("A cluster summary counts by condition", "[search][types]") {
  Lib::ClusterSummary summary;
  summary.clusters.at(static_cast<std::size_t>(Lib::ClusterCondition::Intact)) =
      7;

  CHECK(summary.Count(Lib::ClusterCondition::Intact) == 7);
  CHECK(summary.Count(Lib::ClusterCondition::Bad) == 0);
}
