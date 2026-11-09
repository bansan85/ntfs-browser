#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "scan-support.h"
#include "search/target.h"
#include "utf/utf.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
namespace Utf = NTFS_BROWSER_NAMESPACE::Utf;
using Parts = std::vector<std::wstring>;

namespace {

// The target text of an image with inside after it, as a wide string.
std::wstring Target(const TestSearch::TempImage& image,
                    const std::u16string& inside) {
  return Utf::Utf16ToWide(image.Target(inside));
}

}  // namespace

TEST_CASE("SplitTarget finds the image and the path inside it",
          "[search][target]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());

  SECTION("no inside path") {
    const auto parts = Lib::SplitTarget(Target(image, u""));

    REQUIRE(parts.has_value());
    CHECK(parts->kind == Lib::TargetParts::Kind::Image);
    CHECK(parts->inside.empty());
  }

  SECTION("a path of two folders") {
    const auto parts = Lib::SplitTarget(Target(image, u"Docs/Sub"));

    REQUIRE(parts.has_value());
    CHECK(parts->kind == Lib::TargetParts::Kind::Image);
    CHECK(parts->inside == Parts{L"Docs", L"Sub"});
  }

  SECTION("empty parts and trailing separators are dropped") {
    const auto parts = Lib::SplitTarget(Target(image, u"/Docs///Sub//"));

    REQUIRE(parts.has_value());
    CHECK(parts->inside == Parts{L"Docs", L"Sub"});
  }

  SECTION("a backslash separates the folders inside the volume") {
    const auto parts = Lib::SplitTarget(Target(image, u"Docs") + L"\\Sub");

    REQUIRE(parts.has_value());
    CHECK(parts->inside == Parts{L"Docs", L"Sub"});
  }
}

TEST_CASE("SplitTarget finds nothing in a target that names nothing",
          "[search][target]") {
  CHECK_FALSE(Lib::SplitTarget(L"").has_value());
  CHECK_FALSE(Lib::SplitTarget(L"no-such-file.img").has_value());
#ifndef _WIN32
  CHECK_FALSE(Lib::SplitTarget(L"/no/such/file.img/Docs").has_value());
#endif
}

#ifdef _WIN32

TEST_CASE("SplitTarget reads a drive letter", "[search][target]") {
  SECTION("a bare drive") {
    const auto parts = Lib::SplitTarget(L"z:");

    REQUIRE(parts.has_value());
    CHECK(parts->kind == Lib::TargetParts::Kind::Drive);
    CHECK(parts->source == L"z");
    CHECK(parts->inside.empty());
  }

  SECTION("a drive and a path") {
    const auto parts = Lib::SplitTarget(L"z:\\Windows/System32");

    REQUIRE(parts.has_value());
    CHECK(parts->kind == Lib::TargetParts::Kind::Drive);
    CHECK(parts->inside == Parts{L"Windows", L"System32"});
  }

  SECTION("a colon after a digit is no drive") {
    CHECK_FALSE(Lib::SplitTarget(L"1:\\x").has_value());
  }
}

#endif
