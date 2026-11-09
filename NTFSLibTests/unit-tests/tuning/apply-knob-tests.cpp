#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "tuning/knobs.h"
#include "tuning/tuning.h"
#include "volume-options.h"

namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

constexpr std::array<Tuning::Pattern, Tuning::pattern_count> all_patterns{
    Tuning::Pattern::LargeSequential, Tuning::Pattern::ScatteredGrouped,
    Tuning::Pattern::DependentSmall, Tuning::Pattern::LargeData};

// The value of every knob for every pattern, in knob order: two options are
// the same when their snapshots are.
std::vector<std::uint64_t> Snapshot(const VolumeOptions& options) {
  std::vector<std::uint64_t> values;
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    for (const Tuning::Pattern pattern : all_patterns) {
      values.push_back(knob.get(options, pattern));
    }
  }
  return values;
}

bool Serves(const Tuning::Knob& knob, Tuning::Pattern pattern) {
  return (knob.patterns & (1U << static_cast<unsigned>(pattern))) != 0;
}

// A value of knob that is not its default, within its bounds.
std::uint64_t OtherValue(const Tuning::Knob& knob) {
  return knob.default_value == knob.min_value ? knob.max_value : knob.min_value;
}

// Whether a knob read back as value. A size_t field of a 32-bit build holds at
// most 2^32 - 1 and a larger value is cut there, while a 64-bit field keeps it.
bool ReadsBackAs(std::uint64_t read, std::uint64_t value) {
  return read == value || (value > std::numeric_limits<std::size_t>::max() &&
                           read == std::numeric_limits<std::size_t>::max());
}

}  // namespace

TEST_CASE("ApplyKnob refuses an unknown name and changes nothing",
          "[tuning][apply-knob]") {
  VolumeOptions options;
  const std::vector<std::uint64_t> before = Snapshot(options);

  for (const std::string_view name :
       {"", "reader", "reader.max_reads ", "READER.MAX_READS", "mft.nothing",
        "tuning.mft.window_bytes", "scan.max_hold", "include_deleted"}) {
    INFO("name \"" << name << "\"");
    const std::string failure = Tuning::ApplyKnob(options, name, 1);
    CHECK_THAT(failure, ContainsSubstring("unknown key"));
    CHECK_THAT(failure, ContainsSubstring('"' + std::string(name) + '"'));
  }
  CHECK(Snapshot(options) == before);
}

TEST_CASE("ApplyKnob refuses a value outside the bounds of every knob",
          "[tuning][apply-knob]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    INFO("knob " << knob.name);
    VolumeOptions options;
    const std::vector<std::uint64_t> before = Snapshot(options);

    if (knob.min_value > 0) {
      const std::string failure =
          Tuning::ApplyKnob(options, knob.name, knob.min_value - 1);
      CHECK_THAT(failure, ContainsSubstring(std::string(knob.name)));
      CHECK_THAT(failure,
                 ContainsSubstring("outside " + std::to_string(knob.min_value) +
                                   " to " + std::to_string(knob.max_value)));
      CHECK(Snapshot(options) == before);
    }
    const std::string above =
        Tuning::ApplyKnob(options, knob.name, knob.max_value + 1);
    CHECK_THAT(above, ContainsSubstring("outside"));
    CHECK(Snapshot(options) == before);
    CHECK_FALSE(Tuning::ApplyKnob(options, knob.name,
                                  std::numeric_limits<std::uint64_t>::max())
                    .empty());
    CHECK(Snapshot(options) == before);
    // A value out of range is refused whatever the pattern.
    for (const Tuning::Pattern pattern : all_patterns) {
      if (Serves(knob, pattern)) {
        CHECK_FALSE(
            Tuning::ApplyKnob(options, knob.name, knob.max_value + 1, pattern)
                .empty());
      }
    }
    CHECK(Snapshot(options) == before);
  }
}

TEST_CASE("ApplyKnob accepts both bounds of every knob",
          "[tuning][apply-knob]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    INFO("knob " << knob.name);
    for (const std::uint64_t value : {knob.min_value, knob.max_value}) {
      INFO("value " << value);
      VolumeOptions options;
      CHECK(Tuning::ApplyKnob(options, knob.name, value).empty());
      for (const Tuning::Pattern pattern : all_patterns) {
        if (Serves(knob, pattern)) {
          CHECK(ReadsBackAs(knob.get(options, pattern), value));
        }
      }
    }
  }
}

TEST_CASE("ApplyKnob refuses a pattern the knob does not serve",
          "[tuning][apply-knob]") {
  std::size_t refused = 0;
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    for (const Tuning::Pattern pattern : all_patterns) {
      INFO("knob " << knob.name << ", pattern "
                   << Tuning::PatternName(pattern));
      VolumeOptions options;
      const std::vector<std::uint64_t> before = Snapshot(options);
      const std::string failure =
          Tuning::ApplyKnob(options, knob.name, OtherValue(knob), pattern);
      if (Serves(knob, pattern)) {
        CHECK(failure.empty());
        continue;
      }
      refused++;
      CHECK(failure == "\"" + std::string(knob.name) +
                           "\" does not serve pattern " +
                           std::string(Tuning::PatternName(pattern)));
      CHECK(Snapshot(options) == before);
    }
  }
  // The table has knobs that serve one pattern only.
  CHECK(refused > 0);
}

TEST_CASE("A reader knob serves every pattern", "[tuning][apply-knob]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    if (knob.name.starts_with("reader.")) {
      INFO("knob " << knob.name);
      CHECK(knob.per_pattern);
      CHECK(knob.patterns == 0b1111U);
      CHECK(knob.family == Tuning::Family::PrefetchDepth);
    } else {
      CHECK_FALSE(knob.per_pattern);
    }
  }
}

TEST_CASE("A per-pattern knob without a pattern sets all four",
          "[tuning][apply-knob]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    if (!knob.per_pattern) {
      continue;
    }
    INFO("knob " << knob.name);
    VolumeOptions options;
    const std::uint64_t value = OtherValue(knob);
    REQUIRE(Tuning::ApplyKnob(options, knob.name, value).empty());
    for (const Tuning::Pattern pattern : all_patterns) {
      CHECK(knob.get(options, pattern) == value);
    }
  }
}

TEST_CASE("A per-pattern knob with a pattern sets that pattern only",
          "[tuning][apply-knob]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    if (!knob.per_pattern) {
      continue;
    }
    for (const Tuning::Pattern target : all_patterns) {
      INFO("knob " << knob.name << ", pattern " << Tuning::PatternName(target));
      VolumeOptions options;
      const std::uint64_t value = OtherValue(knob);
      REQUIRE(Tuning::ApplyKnob(options, knob.name, value, target).empty());
      for (const Tuning::Pattern pattern : all_patterns) {
        CHECK(knob.get(options, pattern) ==
              (pattern == target ? value : knob.default_value));
      }
    }
  }
}

TEST_CASE("A knob that no pattern splits is set whatever pattern names it",
          "[tuning][apply-knob]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    if (knob.per_pattern) {
      continue;
    }
    for (const Tuning::Pattern pattern : all_patterns) {
      if (!Serves(knob, pattern)) {
        continue;
      }
      INFO("knob " << knob.name << ", pattern "
                   << Tuning::PatternName(pattern));
      VolumeOptions options;
      const std::uint64_t value = OtherValue(knob);
      REQUIRE(Tuning::ApplyKnob(options, knob.name, value, pattern).empty());
      CHECK(ReadsBackAs(knob.get(options, pattern), value));
    }
  }
}

TEST_CASE("A knob changes its own field and no other", "[tuning][apply-knob]") {
  const VolumeOptions defaults;
  const std::vector<std::uint64_t> base = Snapshot(defaults);
  const std::span<const Tuning::Knob> knobs = Tuning::Knobs();

  for (std::size_t index = 0; index < knobs.size(); index++) {
    const Tuning::Knob& knob = knobs.subspan(index, 1).front();
    INFO("knob " << knob.name);
    VolumeOptions options;
    REQUIRE(Tuning::ApplyKnob(options, knob.name, OtherValue(knob)).empty());
    const std::vector<std::uint64_t> after = Snapshot(options);
    REQUIRE(after.size() == base.size());
    for (std::size_t slot = 0; slot < after.size(); slot++) {
      // Four values per knob in the snapshot.
      if (slot / Tuning::pattern_count != index) {
        CHECK(after.at(slot) == base.at(slot));
      }
    }
  }
}

TEST_CASE("A size knob keeps what a size_t holds", "[tuning][apply-knob]") {
  VolumeOptions options;
  const Tuning::Knob* const knob = Tuning::FindKnob("mft.window_bytes");
  REQUIRE(knob != nullptr);

  // 2^40 is the largest size a profile can name.
  REQUIRE(
      Tuning::ApplyKnob(options, "mft.window_bytes", std::uint64_t{1} << 40U)
          .empty());
  if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
    CHECK(options.tuning.mft.window_bytes == std::uint64_t{1} << 40U);
  } else {
    // A 32-bit size_t is cut at its largest value, not wrapped to 0.
    CHECK(options.tuning.mft.window_bytes ==
          std::numeric_limits<std::size_t>::max());
  }

  // A value that fits is kept exactly, on every platform.
  REQUIRE(Tuning::ApplyKnob(options, "mft.window_bytes", 8192).empty());
  CHECK(options.tuning.mft.window_bytes == 8192);
}

TEST_CASE("The patterns of the knob table are the patterns of their fields",
          "[tuning][apply-knob]") {
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    INFO("knob " << knob.name);
    CHECK(knob.patterns != 0);
    CHECK((knob.patterns & ~0b1111U) == 0);
    CHECK_FALSE(Tuning::FamilyName(knob.family).empty());
    CHECK(Tuning::FamilyName(knob.family) != "?");
  }
  CHECK(Tuning::FamilyName(Tuning::Family::PrefetchDepth) == "A");
  CHECK(Tuning::FamilyName(Tuning::Family::ReadShape) == "B");
  CHECK(Tuning::FamilyName(Tuning::Family::CpuMemory) == "C");
  CHECK(Tuning::FindKnob("nothing.here") == nullptr);
  CHECK(Tuning::PatternName(Tuning::Pattern::LargeSequential) == "M1");
  CHECK(Tuning::PatternName(Tuning::Pattern::LargeData) == "M4");
}
