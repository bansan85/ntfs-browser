#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "upcase/upcase.h"

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::Tests::NonAsciiNameLayout;

namespace {

// Looks name up in the fixture's root directory, and returns the MFT
// reference of the entry FindSubEntry() reports, if any.
template <Cache::Strategy S>
std::optional<ULONGLONG> FindInRoot(NonAsciiNameLayout layout,
                                    bool with_up_case, std::wstring_view name) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNonAsciiNames(
              layout, with_up_case));

  // The named records are not built: only recover_errors lists these entries.
  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
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

}  // namespace

TEST_CASE(
    "FindSubEntry finds a non-ASCII name that sorts after another one "
    "without a $UpCase table",
    "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    const std::optional<ULONGLONG> found = FindInRoot<S>(
        layout, false, NTFS_BROWSER_NAMESPACE::Tests::non_ascii_diaeresis_name);
    REQUIRE(found.has_value());
    CHECK(*found == NTFS_BROWSER_NAMESPACE::Tests::non_ascii_diaeresis_mft_ref);
  });
}

TEST_CASE(
    "FindSubEntry finds a non-ASCII name that sorts after another one "
    "with a $UpCase table",
    "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    const std::optional<ULONGLONG> found = FindInRoot<S>(
        layout, true, NTFS_BROWSER_NAMESPACE::Tests::non_ascii_diaeresis_name);
    REQUIRE(found.has_value());
    CHECK(*found == NTFS_BROWSER_NAMESPACE::Tests::non_ascii_diaeresis_mft_ref);
  });
}

TEST_CASE("FindSubEntry matches a non-ASCII name case-insensitively",
          "[file-record][filename][upcase][regression]") {
  for (const bool with_up_case : {false, true}) {
    ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
      const std::optional<ULONGLONG> found = FindInRoot<S>(
          layout, with_up_case,
          NTFS_BROWSER_NAMESPACE::Tests::non_ascii_acute_upper_name);
      REQUIRE(found.has_value());
      CHECK(*found == NTFS_BROWSER_NAMESPACE::Tests::non_ascii_acute_mft_ref);
    });
  }
}

TEST_CASE("FindSubEntry follows the volume's own $UpCase table",
          "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    const std::optional<ULONGLONG> dotless = FindInRoot<S>(
        layout, true, NTFS_BROWSER_NAMESPACE::Tests::non_ascii_dotless_name);
    REQUIRE(dotless.has_value());
    CHECK(*dotless == NTFS_BROWSER_NAMESPACE::Tests::non_ascii_dotless_mft_ref);

    // The table leaves the dotless i unmapped: "I.txt" is another name.
    CHECK_FALSE(FindInRoot<S>(
                    layout, true,
                    NTFS_BROWSER_NAMESPACE::Tests::non_ascii_dotted_upper_name)
                    .has_value());
  });
}

TEST_CASE(
    "FindSubEntry scans every entry when the case mapping is only built in",
    "[file-record][filename][upcase][regression]") {
  ForEachLayoutAndStrategy([&]<Cache::Strategy S>(NonAsciiNameLayout layout) {
    // Without a $UpCase table, the built-in mapping folds the dotless i
    // to I, so the ordered search stops before it. The scan finds it.
    const std::optional<ULONGLONG> found = FindInRoot<S>(
        layout, false, NTFS_BROWSER_NAMESPACE::Tests::non_ascii_dotless_name);
    REQUIRE(found.has_value());
    CHECK(*found == NTFS_BROWSER_NAMESPACE::Tests::non_ascii_dotless_mft_ref);
  });
}
