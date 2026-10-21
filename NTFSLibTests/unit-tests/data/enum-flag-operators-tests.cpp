#include <ntfs-browser/win-types.h>

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

namespace {

// A scoped enum whose bit operators come from DEFINE_ENUM_FLAG_OPERATORS.
enum class Probe : std::uint8_t { None = 0, Low = 1, Mid = 2, High = 4 };

}  // namespace

DEFINE_ENUM_FLAG_OPERATORS(Probe)

TEST_CASE("DEFINE_ENUM_FLAG_OPERATORS gives a scoped enum every bit operator",
          "[data][enum-flags]") {
  SECTION("or and and") {
    Probe flags = Probe::Low | Probe::High;
    CHECK((flags & Probe::High) == Probe::High);
    CHECK((flags & Probe::Mid) == Probe::None);

    flags |= Probe::Mid;
    CHECK((flags & Probe::Mid) == Probe::Mid);

    flags &= Probe::Mid | Probe::High;
    CHECK(flags == (Probe::Mid | Probe::High));
  }

  SECTION("xor and complement") {
    CHECK((Probe::Low ^ Probe::Low) == Probe::None);
    CHECK((Probe::Low ^ Probe::Mid) == (Probe::Low | Probe::Mid));

    Probe flags = Probe::Low | Probe::Mid;
    flags ^= Probe::Low;
    CHECK(flags == Probe::Mid);

    CHECK((~Probe::Low & (Probe::Low | Probe::Mid)) == Probe::Mid);
  }
}
