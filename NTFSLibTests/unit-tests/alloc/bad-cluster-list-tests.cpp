#include <ntfs-browser/win-types.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "alloc/bad-cluster-list.h"
#include "test-log-sink.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
using Alloc::BadClusterList;
using Alloc::Extent;
using Alloc::StreamExtents;

namespace {

// A whole stream of the given extents.
StreamExtents Stream(std::vector<Extent> extents, bool complete = true) {
  return {.extents = std::move(extents), .complete = complete};
}

// Restores the default ceiling when a test case ends.
struct CeilingGuard {
  explicit CeilingGuard(size_t ceiling) {
    Alloc::SetBadExtentCeilingForTests(ceiling);
  }

  CeilingGuard(const CeilingGuard&) = delete;
  CeilingGuard& operator=(const CeilingGuard&) = delete;

  ~CeilingGuard() { Alloc::SetBadExtentCeilingForTests(0); }
};

// The extents of a built list as {lcn, clusters} pairs, for a short check.
std::vector<std::pair<ULONGLONG, ULONGLONG>> Pairs(const BadClusterList& list) {
  std::vector<std::pair<ULONGLONG, ULONGLONG>> pairs;
  for (const Extent& extent : list.Get().extents) {
    pairs.emplace_back(extent.lcn, extent.clusters);
  }
  return pairs;
}

// A list of count isolated one-cluster extents, two clusters apart.
std::vector<Extent> Isolated(size_t count) {
  std::vector<Extent> extents;
  extents.reserve(count);
  for (size_t i = 0; i < count; i++) {
    extents.push_back({.lcn = 2 * ULONGLONG{i}, .clusters = 1});
  }
  return extents;
}

}  // namespace

TEST_CASE("an empty stream gives a present, empty list", "[alloc][bad-list]") {
  const auto list = BadClusterList::Build(Stream({}), 100, false);

  REQUIRE(list != nullptr);
  CHECK(list->Get().extents.empty());
  CHECK_FALSE(list->Get().truncated);
}

TEST_CASE("extents come out sorted and merged", "[alloc][bad-list]") {
  SECTION("out of order") {
    const auto list =
        BadClusterList::Build(Stream({{.lcn = 50, .clusters = 2},
                                      {.lcn = 10, .clusters = 3},
                                      {.lcn = 30, .clusters = 1}}),
                              100, false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) == std::vector<std::pair<ULONGLONG, ULONGLONG>>{
                              {10, 3}, {30, 1}, {50, 2}});
  }

  SECTION("touching extents join") {
    const auto list = BadClusterList::Build(
        Stream({{.lcn = 10, .clusters = 5}, {.lcn = 15, .clusters = 3}}), 100,
        false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{10, 8}});
  }

  SECTION("overlapping extents join") {
    const auto list = BadClusterList::Build(
        Stream({{.lcn = 10, .clusters = 5}, {.lcn = 12, .clusters = 6}}), 100,
        false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{10, 8}});
  }

  SECTION("an extent inside another disappears") {
    const auto list = BadClusterList::Build(
        Stream({{.lcn = 10, .clusters = 10}, {.lcn = 12, .clusters = 2}}), 100,
        false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{10, 10}});
  }

  SECTION("a zero-length extent is dropped") {
    const auto list = BadClusterList::Build(
        Stream({{.lcn = 10, .clusters = 0}, {.lcn = 20, .clusters = 1}}), 100,
        false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{20, 1}});
  }
}

TEST_CASE("extents are clamped to the volume", "[alloc][bad-list]") {
  SECTION("one wholly past the end is dropped, and logged") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const auto list = BadClusterList::Build(
        Stream({{.lcn = 10, .clusters = 2}, {.lcn = 100, .clusters = 4}}), 100,
        false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{10, 2}});
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog().find(
              "past the volume") != std::string::npos);
  }

  SECTION("one partly past the end is cut") {
    const auto list = BadClusterList::Build(
        Stream({{.lcn = 98, .clusters = 10}}), 100, false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{98, 2}});
  }

  SECTION("an unknown volume clamps nothing") {
    const auto list = BadClusterList::Build(
        Stream({{.lcn = 1000, .clusters = 10}}), 0, false);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{1000, 10}});
  }

  SECTION("an extent that overflows 64 bits is cut, not wrapped") {
    const auto list = BadClusterList::Build(
        Stream({{.lcn = ULONGLONG{0} - 5, .clusters = 100}}), 0, true);

    REQUIRE(list != nullptr);
    REQUIRE(list->Get().extents.size() == 1);
    CHECK(list->Get().extents.front().lcn == ULONGLONG{0} - 5);
    CHECK(list->Get().extents.front().clusters == 4);
  }
}

TEST_CASE("a damaged stream is rejected when strict and kept when recovering",
          "[alloc][bad-list]") {
  const StreamExtents damaged =
      Stream({{.lcn = 10, .clusters = 2}, {.lcn = 20, .clusters = 3}}, false);

  SECTION("strict") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    CHECK(BadClusterList::Build(damaged, 100, false) == nullptr);
    CHECK_FALSE(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog().empty());
  }

  SECTION("recovering") {
    const auto list = BadClusterList::Build(damaged, 100, true);

    REQUIRE(list != nullptr);
    CHECK(Pairs(*list) ==
          std::vector<std::pair<ULONGLONG, ULONGLONG>>{{10, 2}, {20, 3}});
    CHECK_FALSE(list->Get().truncated);
  }
}

TEST_CASE("a list over the ceiling is rejected or cut", "[alloc][bad-list]") {
  const CeilingGuard guard(8);

  SECTION("exactly the ceiling is whole") {
    const auto list = BadClusterList::Build(Stream(Isolated(8)), 0, false);

    REQUIRE(list != nullptr);
    CHECK(list->Get().extents.size() == 8);
    CHECK_FALSE(list->Get().truncated);
  }

  SECTION("strict rejects it, and logs") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    CHECK(BadClusterList::Build(Stream(Isolated(9)), 0, false) == nullptr);
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog().find(
              "more than 8 extents") != std::string::npos);
  }

  SECTION("recovering keeps the lowest and flags it") {
    std::vector<Extent> extents = Isolated(9);
    std::swap(extents.front(), extents.back());
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const auto list =
        BadClusterList::Build(Stream(std::move(extents)), 0, true);

    REQUIRE(list != nullptr);
    CHECK(list->Get().truncated);
    REQUIRE(list->Get().extents.size() == 8);
    CHECK(list->Get().extents.front().lcn == 0);
    CHECK(list->Get().extents.back().lcn == 14);
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog().find(
              "more than 8 extents") != std::string::npos);
  }
}

TEST_CASE("the ceiling counts merged extents", "[alloc][bad-list]") {
  const CeilingGuard guard(2);
  std::vector<Extent> extents;
  for (ULONGLONG i = 0; i < 20; i++) {
    extents.push_back({.lcn = i, .clusters = 1});
  }

  const auto list = BadClusterList::Build(Stream(std::move(extents)), 0, false);

  REQUIRE(list != nullptr);
  CHECK(Pairs(*list) == std::vector<std::pair<ULONGLONG, ULONGLONG>>{{0, 20}});
}

TEST_CASE("the default ceiling holds the worst case of a Windows volume",
          "[alloc][bad-list]") {
  CHECK(Alloc::BadExtentCeiling() == Alloc::max_bad_extents);
  CHECK(Alloc::max_bad_extents == 5242880);
  CHECK(Alloc::max_bad_extents >= 5168739);
}

TEST_CASE("a list is found by position", "[alloc][bad-list]") {
  std::vector<Extent> extents = Isolated(1000);
  const auto list = BadClusterList::Build(Stream(std::move(extents)), 0, false);
  REQUIRE(list != nullptr);

  SECTION("a range that touches two extents finds exactly two") {
    const auto found = list->Overlapping(10, 13);
    REQUIRE(found.size() == 2);
    CHECK(found[0].lcn == 10);
    CHECK(found[1].lcn == 12);
    CHECK(list->CountIn(10, 13) == 2);
  }

  SECTION("a range between two extents finds none") {
    CHECK(list->Overlapping(11, 12).empty());
    CHECK(list->CountIn(11, 12) == 0);
  }

  SECTION("an empty or inverted range finds none") {
    CHECK(list->Overlapping(10, 10).empty());
    CHECK(list->Overlapping(20, 10).empty());
    CHECK(list->CountIn(20, 10) == 0);
  }

  SECTION("a range wider than the list counts every cluster once") {
    CHECK(list->CountIn(0, ULONGLONG{0} - 1) == 1000);
  }

  SECTION("a range cuts an extent") {
    const auto wide =
        BadClusterList::Build(Stream({{.lcn = 100, .clusters = 50}}), 0, false);
    REQUIRE(wide != nullptr);
    CHECK(wide->CountIn(120, 130) == 10);
    CHECK(wide->CountIn(90, 110) == 10);
    CHECK(wide->CountIn(140, 200) == 10);
    CHECK(wide->Overlapping(90, 100).empty());
  }
}

TEST_CASE("a list of the worst case of a Windows volume is kept whole",
          "[alloc][bad-list]") {
  constexpr size_t worst_case = 5168739;
  constexpr size_t eighty_mib = size_t{80} * 1024 * 1024;

  SECTION("strict") {
    const auto list =
        BadClusterList::Build(Stream(Isolated(worst_case)), 0, false);

    REQUIRE(list != nullptr);
    CHECK(list->Get().extents.size() == worst_case);
    CHECK_FALSE(list->Get().truncated);
    CHECK(list->BytesHeld() <= eighty_mib);
  }

  SECTION("recovering") {
    const auto list =
        BadClusterList::Build(Stream(Isolated(worst_case)), 0, true);

    REQUIRE(list != nullptr);
    CHECK(list->Get().extents.size() == worst_case);
    CHECK_FALSE(list->Get().truncated);
    CHECK(list->BytesHeld() <= eighty_mib);
  }
}

TEST_CASE("a list over the default ceiling is rejected or cut",
          "[alloc][bad-list]") {
  constexpr size_t over = Alloc::max_bad_extents + 1;

  SECTION("strict rejects it") {
    CHECK(BadClusterList::Build(Stream(Isolated(over)), 0, false) == nullptr);
  }

  SECTION("recovering cuts it to the ceiling and bounds the memory") {
    const auto list = BadClusterList::Build(Stream(Isolated(over)), 0, true);

    REQUIRE(list != nullptr);
    CHECK(list->Get().truncated);
    CHECK(list->Get().extents.size() == Alloc::max_bad_extents);
    CHECK(list->BytesHeld() <= size_t{80} * 1024 * 1024);
  }
}

TEST_CASE("the memory a list holds is its capacity times an extent",
          "[alloc][bad-list]") {
  const auto list = BadClusterList::Build(Stream(Isolated(1000)), 0, false);

  REQUIRE(list != nullptr);
  CHECK(list->BytesHeld() == 1000 * sizeof(Extent));
}

TEST_CASE("bad list load", "[.benchmark][alloc]") {
  constexpr size_t worst_case = 5168739;
  std::vector<Extent> extents = Isolated(worst_case);

  const auto start = std::chrono::steady_clock::now();
  const auto list = BadClusterList::Build(Stream(std::move(extents)), 0, true);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(list != nullptr);
  std::cout
      << "bad list load: " << worst_case << " extents in "
      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
      << " ms, " << list->BytesHeld() / (1024 * 1024) << " MiB held\n";
}

TEST_CASE("extents that start on the same cluster merge into the longest",
          "[alloc][bad-list]") {
  const auto list = BadClusterList::Build(Stream({{.lcn = 10, .clusters = 5},
                                                  {.lcn = 10, .clusters = 2},
                                                  {.lcn = 10, .clusters = 9}}),
                                          100, false);

  REQUIRE(list != nullptr);
  const std::vector<std::pair<ULONGLONG, ULONGLONG>> expected{{10, 9}};
  CHECK(Pairs(*list) == expected);
}
