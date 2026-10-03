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
#include "upcase.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::UpCaseTable;
using NtfsBrowser::Enum::MftIdx;
using NtfsBrowserTests::NonAsciiNameLayout;

namespace
{

// Selects the low byte of an UpCase unit.
constexpr size_t kLowByteMask = 0xFF;

// The UpCase unit U+00E9, and the one the test maps it to: U+0041.
constexpr size_t kAcuteEUnit = 0xE9;
constexpr BYTE kMappedUnitLow = 0x41;

// Looks name up in the fixture's root directory, and returns the MFT
// reference of the entry FindSubEntry() reports, if any.
template <Strategy S>
std::optional<ULONGLONG> FindInRoot(NonAsciiNameLayout layout, bool withUpCase,
                                    std::wstring_view name)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithNonAsciiNames(layout,
                                                            withUpCase));

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> found = root.FindSubEntry(name);
  if (!found)
  {
    return std::nullopt;
  }
  return found->GetFileReference();
}

// Runs check once per layout and per cache strategy.
template <typename Check>
void ForEachLayoutAndStrategy(Check check)
{
  for (const NonAsciiNameLayout layout :
       {NonAsciiNameLayout::kIndexRoot, NonAsciiNameLayout::kIndexBlock})
  {
    check.template operator()<Strategy::NO_CACHE>(layout);
    check.template operator()<Strategy::FULL_CACHE>(layout);
  }
}

// A well-formed raw $UpCase: the identity map but for a-z.
std::vector<BYTE> MakeMinimalUpCaseBytes()
{
  constexpr size_t kCaseDistance = 0x20;
  constexpr unsigned kBitsPerByte = 8;

  std::vector<BYTE> bytes(NtfsBrowser::kUpCaseByteCount);
  for (size_t unit = 0; unit < NtfsBrowser::kUpCaseUnitCount; unit++)
  {
    const size_t upper =
        (unit >= L'a' && unit <= L'z') ? unit - kCaseDistance : unit;
    // bytes holds 2 bytes for each unit below kUpCaseUnitCount.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[unit * 2] = static_cast<BYTE>(upper & kLowByteMask);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[unit * 2 + 1] = gsl::narrow<BYTE>(upper >> kBitsPerByte);
  }
  return bytes;
}

}  // namespace

TEST_CASE(
    "FindSubEntry finds a non-ASCII name that sorts after another one "
    "without a $UpCase table",
    "[file-record][filename][upcase][regression]")
{
  ForEachLayoutAndStrategy(
      [&]<Strategy S>(NonAsciiNameLayout layout)
      {
        const std::optional<ULONGLONG> found = FindInRoot<S>(
            layout, false, NtfsBrowserTests::kNonAsciiDiaeresisName);
        REQUIRE(found.has_value());
        CHECK(*found == NtfsBrowserTests::kNonAsciiDiaeresisMftRef);
      });
}

TEST_CASE(
    "FindSubEntry finds a non-ASCII name that sorts after another one "
    "with a $UpCase table",
    "[file-record][filename][upcase][regression]")
{
  ForEachLayoutAndStrategy(
      [&]<Strategy S>(NonAsciiNameLayout layout)
      {
        const std::optional<ULONGLONG> found = FindInRoot<S>(
            layout, true, NtfsBrowserTests::kNonAsciiDiaeresisName);
        REQUIRE(found.has_value());
        CHECK(*found == NtfsBrowserTests::kNonAsciiDiaeresisMftRef);
      });
}

TEST_CASE("FindSubEntry matches a non-ASCII name case-insensitively",
          "[file-record][filename][upcase][regression]")
{
  for (const bool withUpCase : {false, true})
  {
    ForEachLayoutAndStrategy(
        [&]<Strategy S>(NonAsciiNameLayout layout)
        {
          const std::optional<ULONGLONG> found = FindInRoot<S>(
              layout, withUpCase, NtfsBrowserTests::kNonAsciiAcuteUpperName);
          REQUIRE(found.has_value());
          CHECK(*found == NtfsBrowserTests::kNonAsciiAcuteMftRef);
        });
  }
}

TEST_CASE("FindSubEntry follows the volume's own $UpCase table",
          "[file-record][filename][upcase][regression]")
{
  ForEachLayoutAndStrategy(
      [&]<Strategy S>(NonAsciiNameLayout layout)
      {
        const std::optional<ULONGLONG> dotless =
            FindInRoot<S>(layout, true, NtfsBrowserTests::kNonAsciiDotlessName);
        REQUIRE(dotless.has_value());
        CHECK(*dotless == NtfsBrowserTests::kNonAsciiDotlessMftRef);

        // The table leaves the dotless i unmapped: "I.txt" is another name.
        CHECK_FALSE(FindInRoot<S>(layout, true,
                                  NtfsBrowserTests::kNonAsciiDottedUpperName)
                        .has_value());
      });
}

TEST_CASE(
    "FindSubEntry scans every entry when the case mapping is only built in",
    "[file-record][filename][upcase][regression]")
{
  ForEachLayoutAndStrategy(
      [&]<Strategy S>(NonAsciiNameLayout layout)
      {
        // Without a $UpCase table, the built-in mapping folds the dotless i
        // to I, so the ordered search stops before it. The scan finds it.
        const std::optional<ULONGLONG> found = FindInRoot<S>(
            layout, false, NtfsBrowserTests::kNonAsciiDotlessName);
        REQUIRE(found.has_value());
        CHECK(*found == NtfsBrowserTests::kNonAsciiDotlessMftRef);
      });
}

TEST_CASE("The built-in case mapping is the Unicode simple uppercase mapping",
          "[filename][upcase][regression]")
{
  const UpCaseTable& table = UpCaseTable::BuiltIn();
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
          "[filename][upcase][regression]")
{
  const std::vector<BYTE> good = MakeMinimalUpCaseBytes();

  SECTION("a well-formed stream is used as it is")
  {
    std::vector<BYTE> bytes = good;
    // Maps U+00E9 to U+0041.
    // kAcuteEUnit is below kUpCaseUnitCount, so both of its bytes are in bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[kAcuteEUnit * 2] = kMappedUnitLow;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[kAcuteEUnit * 2 + 1] = 0x00;

    const std::optional<UpCaseTable> table = UpCaseTable::FromBytes(bytes);
    REQUIRE(table.has_value());
    CHECK_FALSE(table->IsBuiltIn());
    CHECK(table->Map(u'a') == u'A');
    CHECK(table->Map(u'\u00E9') == u'A');
    CHECK(table->Map(u'\u00C9') == u'\u00C9');
  }

  SECTION("a short stream is refused")
  {
    const std::vector<BYTE> bytes(good.begin(), good.end() - 2);
    CHECK_FALSE(UpCaseTable::FromBytes(bytes).has_value());
  }

  SECTION("a wiped stream is refused")
  {
    const std::vector<BYTE> bytes(good.size(), 0);
    CHECK_FALSE(UpCaseTable::FromBytes(bytes).has_value());
  }
}

TEST_CASE("Case tables collate names unit by unit through their mapping",
          "[filename][upcase][regression]")
{
  const UpCaseTable& table = UpCaseTable::BuiltIn();

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
          "[filename][upcase][regression]")
{
  // U+1F600 is the pair D83D DE00 in UTF-16, which sorts before U+FFFD,
  // although the code point itself is the larger one.
  const UpCaseTable& table = UpCaseTable::BuiltIn();
  CHECK(table.Compare(L"\U0001F600", L"\uFFFD") < 0);
  CHECK(table.Compare(L"\uFFFD", L"\U0001F600") > 0);
  CHECK(table.Compare(L"\U0001F600", L"\U0001F600") == 0);
}
