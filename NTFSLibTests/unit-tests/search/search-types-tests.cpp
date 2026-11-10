#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

namespace Lib = NTFS_BROWSER_NAMESPACE;

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

TEST_CASE("A name converts to UTF-8", "[search][types]") {
  CHECK(Lib::ToUtf8(u"Docs") == "Docs");
  CHECK(Lib::ToUtf8(u"é") == "\xC3\xA9");
  CHECK(Lib::ToUtf8(u"\U0001F600") == "\xF0\x9F\x98\x80");
  CHECK(Lib::ToUtf8(u"\xD800") == "\xED\xA0\x80");
}

TEST_CASE("An entry shows its flags as letters", "[search][types]") {
  Lib::Entry entry;
  CHECK(Lib::EntryFlags(entry) == "-------");
  entry.is_directory = true;
  entry.attributes = Lib::Attribute::read_only | Lib::Attribute::encrypted;
  CHECK(Lib::EntryFlags(entry) == "DR----E");
  entry.is_directory = false;
  entry.attributes = Lib::Attribute::hidden | Lib::Attribute::system |
                     Lib::Attribute::archive | Lib::Attribute::compressed;
  CHECK(Lib::EntryFlags(entry) == "--HSAC-");
}
