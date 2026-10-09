#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"
#include "upcase.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowser::UpCase::Table;
using NtfsBrowserTests::NonAsciiNameLayout;

namespace {

// Selects the low byte of an UpCase unit.
constexpr size_t low_byte_mask = 0xFF;

// The UpCase unit U+00E9, and the one the test maps it to: U+0041.
constexpr size_t acute_e_unit = 0xE9;
constexpr BYTE mapped_unit_low = 0x41;

// Looks name up in the fixture's root directory, and returns the MFT
// reference of the entry FindSubEntry() reports, if any.
template <Cache::Strategy S>
std::optional<ULONGLONG> FindInRoot(NonAsciiNameLayout layout,
                                    bool with_up_case, std::wstring_view name) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithNonAsciiNames(layout,
                                                            with_up_case));

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> found = root.FindSubEntry(name);
  if (!found) {
    return std::nullopt;
  }
  return found->GetFileReference();
}

// Runs check once per layout and per cache strategy.
template <typename Check>
void ForEachLayoutAndStrategy(Check check) {
  for (const NonAsciiNameLayout layout :
       {NonAsciiNameLayout::IndexRoot, NonAsciiNameLayout::IndexBlock}) {
    check.template operator()<Cache::Strategy::NoCache>(layout);
    check.template operator()<Cache::Strategy::FullCache>(layout);
  }
}

// A well-formed raw $UpCase: the identity map but for a-z.
std::vector<BYTE> MakeMinimalUpCaseBytes() {
  constexpr size_t case_distance = 0x20;
  constexpr unsigned bits_per_byte = 8;

  std::vector<BYTE> bytes(NtfsBrowser::UpCase::Table::byte_count);
  for (size_t unit = 0; unit < NtfsBrowser::UpCase::Table::unit_count; unit++) {
    const size_t upper =
        (unit >= L'a' && unit <= L'z') ? unit - case_distance : unit;
    // bytes holds 2 bytes for each unit below UpCaseTable::unit_count.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[unit * 2] = static_cast<BYTE>(upper & low_byte_mask);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[unit * 2 + 1] = gsl::narrow<BYTE>(upper >> bits_per_byte);
  }
  return bytes;
}

}  // namespace

TEST_CASE(
    "FindSubEntry finds a non-ASCII name that sorts after another one "
    "without a $UpCase table",
    "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    const std::optional<ULONGLONG> found = FindInRoot<S>(
        layout, false, NtfsBrowserTests::non_ascii_diaeresis_name);
    REQUIRE(found.has_value());
    CHECK(*found == NtfsBrowserTests::non_ascii_diaeresis_mft_ref);
  });
}

TEST_CASE(
    "FindSubEntry finds a non-ASCII name that sorts after another one "
    "with a $UpCase table",
    "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    const std::optional<ULONGLONG> found =
        FindInRoot<S>(layout, true, NtfsBrowserTests::non_ascii_diaeresis_name);
    REQUIRE(found.has_value());
    CHECK(*found == NtfsBrowserTests::non_ascii_diaeresis_mft_ref);
  });
}

TEST_CASE("FindSubEntry matches a non-ASCII name case-insensitively",
          "[file-record][filename][upcase][regression]") {
  for (const bool with_up_case : {false, true}) {
    ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
      const std::optional<ULONGLONG> found = FindInRoot<S>(
          layout, with_up_case, NtfsBrowserTests::non_ascii_acute_upper_name);
      REQUIRE(found.has_value());
      CHECK(*found == NtfsBrowserTests::non_ascii_acute_mft_ref);
    });
  }
}

TEST_CASE("FindSubEntry follows the volume's own $UpCase table",
          "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    const std::optional<ULONGLONG> dotless =
        FindInRoot<S>(layout, true, NtfsBrowserTests::non_ascii_dotless_name);
    REQUIRE(dotless.has_value());
    CHECK(*dotless == NtfsBrowserTests::non_ascii_dotless_mft_ref);

    // The table leaves the dotless i unmapped: "I.txt" is another name.
    CHECK_FALSE(FindInRoot<S>(layout, true,
                              NtfsBrowserTests::non_ascii_dotted_upper_name)
                    .has_value());
  });
}

TEST_CASE(
    "FindSubEntry scans every entry when the case mapping is only built in",
    "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    // Without a $UpCase table, the built-in mapping folds the dotless i
    // to I, so the ordered search stops before it. The scan finds it.
    const std::optional<ULONGLONG> found =
        FindInRoot<S>(layout, false, NtfsBrowserTests::non_ascii_dotless_name);
    REQUIRE(found.has_value());
    CHECK(*found == NtfsBrowserTests::non_ascii_dotless_mft_ref);
  });
}

TEST_CASE("The built-in case mapping is the Unicode simple uppercase mapping",
          "[filename][upcase][regression]") {
  const Table& table = Table::BuiltIn();
  CHECK(table.IsBuiltIn());

  CHECK(table.Map(u'a') == u'A');
  CHECK(table.Map(u'Z') == u'Z');
  CHECK(table.Map(u'\u00E9') == u'\u00C9');
  CHECK(table.Map(u'\u00FF') == u'\u0178');
  CHECK(table.Map(u'\u03C9') == u'\u03A9');
  CHECK(table.Map(u'\u0436') == u'\u0416');
  CHECK(table.Map(u'\u0101') == u'\u0100');
  CHECK(table.Map(u'7') == u'7');
  CHECK(table.Map(u'_') == u'_');
  // No single-unit uppercase: sharp s stays as it is.
  CHECK(table.Map(u'\u00DF') == u'\u00DF');
  CHECK(table.Map(static_cast<char16_t>(0xD83D)) ==
        static_cast<char16_t>(0xD83D));
}

TEST_CASE("Building a case table from $UpCase bytes checks what it is given",
          "[filename][upcase][regression]") {
  const std::vector<BYTE> good = MakeMinimalUpCaseBytes();

  SECTION("a well-formed stream is used as it is") {
    std::vector<BYTE> bytes = good;
    // Maps U+00E9 to U+0041.
    // acute_e_unit is below UpCaseTable::unit_count, so both of its bytes are
    // in bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[acute_e_unit * 2] = mapped_unit_low;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[acute_e_unit * 2 + 1] = 0x00;

    const std::optional<Table> table = Table::FromBytes(bytes);
    REQUIRE(table.has_value());
    CHECK_FALSE(NtfsBrowserTests::Unwrap(table).IsBuiltIn());
    CHECK(NtfsBrowserTests::Unwrap(table).Map(u'a') == u'A');
    CHECK(NtfsBrowserTests::Unwrap(table).Map(u'\u00E9') == u'A');
    CHECK(NtfsBrowserTests::Unwrap(table).Map(u'\u00C9') == u'\u00C9');
  }

  SECTION("a short stream is refused") {
    const std::vector<BYTE> bytes(good.begin(), good.end() - 2);
    CHECK_FALSE(Table::FromBytes(bytes).has_value());
  }

  SECTION("a wiped stream is refused") {
    const std::vector<BYTE> bytes(good.size(), 0);
    CHECK_FALSE(Table::FromBytes(bytes).has_value());
  }
}

TEST_CASE("Case tables collate names unit by unit through their mapping",
          "[filename][upcase][regression]") {
  const Table& table = Table::BuiltIn();

  CHECK(table.Compare(L"abc", L"ABC") == 0);
  CHECK(table.Compare(L"\u00E9.txt", L"\u00C9.TXT") == 0);
  CHECK(table.Compare(L"\u00C9", L"\u00D6") < 0);
  CHECK(table.Compare(L"\u00D6", L"\u00E9") > 0);
  CHECK(table.Compare(L"Sys", L"System") < 0);
  CHECK(table.Compare(L"System", L"Sys") > 0);
  CHECK(table.Compare(L"", L"") == 0);
  CHECK(table.Compare(L"", L"a") < 0);
}

TEST_CASE("Case tables collate a supplementary code point as a surrogate pair",
          "[filename][upcase][regression]") {
  // U+1F600 is the pair D83D DE00 in UTF-16, which sorts before U+FFFD,
  // although the code point itself is the larger one.
  const Table& table = Table::BuiltIn();
  CHECK(table.Compare(L"\U0001F600", L"\uFFFD") < 0);
  CHECK(table.Compare(L"\uFFFD", L"\U0001F600") > 0);
  CHECK(table.Compare(L"\U0001F600", L"\U0001F600") == 0);
}
