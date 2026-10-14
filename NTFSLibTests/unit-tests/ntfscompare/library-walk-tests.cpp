#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/ntfs-volume.h>

#include "entry.h"
#include "fake-ntfs-image.h"
#include "library-walk.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Cache::Strategy;
using NtfsBrowser::Mft::MftTree;
using NtfsBrowserTests::BuildFakeNtfsImageWithDumpTree;
using NtfsBrowserTests::BuildFakeNtfsImageWithMftTree;
using NtfsBrowserTests::FakeDataRun;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::Unwrap;
using NtfsCompare::Entry;
using NtfsCompare::Listing;
using NtfsCompare::ResolveDirectoryRecord;
using NtfsCompare::WalkLibraryIndex;
using NtfsCompare::WalkMftTree;

namespace {

// Opens a fake image as a volume with cache strategy S.
template <Strategy S>
std::unique_ptr<NtfsVolume<S>> OpenVolume(std::vector<BYTE> image) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

// The dump-tree image, whose directories carry real index blocks. Its root
// (#5) indexes "big.bin" (6), "Docs" (16) and "empty.txt" (7). Docs (16)
// indexes "Broken" (25), "OldDir" (20), "old.tmp" (19) and "report.txt" (17).
template <Strategy S>
std::unique_ptr<NtfsVolume<S>> OpenIndexedVolume() {
  return OpenVolume<S>(BuildFakeNtfsImageWithDumpTree(
      NtfsBrowserTests::dump_big_file_size,
      {FakeDataRun{.lcn = NtfsBrowserTests::dump_big_file_lcn,
                   .clusters = NtfsBrowserTests::dump_big_file_clusters}}));
}

// The record number of the volume's root directory.
constexpr ULONGLONG root_record =
    static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::Root);

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "ResolveDirectoryRecord finds the record of a directory "
    "path",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();

  CHECK(ResolveDirectoryRecord(*volume, L"") == root_record);
  CHECK(ResolveDirectoryRecord(*volume, L"Docs") ==
        NtfsBrowserTests::mft_tree_docs_idx);
  CHECK(ResolveDirectoryRecord(*volume, L"Docs/") ==
        NtfsBrowserTests::mft_tree_docs_idx);
  CHECK(ResolveDirectoryRecord(*volume, L"/Docs") ==
        NtfsBrowserTests::mft_tree_docs_idx);
}

TEMPLATE_TEST_CASE_SIG(
    "ResolveDirectoryRecord finds nothing for a missing or "
    "non-directory path",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();

  CHECK_FALSE(ResolveDirectoryRecord(*volume, L"Docs/missing").has_value());
  CHECK_FALSE(ResolveDirectoryRecord(*volume, L"Docs/report.txt").has_value());
  CHECK_FALSE(ResolveDirectoryRecord(*volume, L"big.bin").has_value());
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex lists the indexed entries below the "
    "root, freed ones included",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();

  const Listing listing = WalkLibraryIndex(*volume, root_record);

  // big.bin and empty.txt are below the first user record, so both are left
  // out as system metafiles.
  CHECK(listing.size() == 5);
  for (const std::wstring_view key : {L"Docs", L"Docs/Broken", L"Docs/OldDir",
                                      L"Docs/old.tmp", L"Docs/report.txt"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
  REQUIRE(listing.contains(L"Docs"));
  CHECK(listing.at(L"Docs").is_directory);
  REQUIRE(listing.contains(L"Docs/report.txt"));
  CHECK_FALSE(listing.at(L"Docs/report.txt").is_directory);
  CHECK(listing.at(L"Docs/report.txt").physical_size.has_value());
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex from a directory lists only its "
    "subtree, by bare names",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();

  const Listing listing =
      WalkLibraryIndex(*volume, NtfsBrowserTests::mft_tree_docs_idx);

  CHECK(listing.size() == 4);
  for (const std::wstring_view key :
       {L"Broken", L"OldDir", L"old.tmp", L"report.txt"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
}

TEMPLATE_TEST_CASE_SIG("WalkMftTree lists the live tree below the root",
                       "[ntfscompare][library-walk][characterization]",
                       ((Strategy S), S), Strategy::NoCache,
                       Strategy::FullCache) {
  const auto volume = OpenVolume<S>(BuildFakeNtfsImageWithMftTree());
  const MftTree tree(*volume);

  const Listing listing = WalkMftTree(tree, root_record);

  CHECK(listing.size() == 5);
  for (const std::wstring_view key :
       {L"Docs", L"Docs/report.txt", L"Docs/link-b", L"link-a", L"NewDir"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
  REQUIRE(listing.contains(L"Docs/report.txt"));
  CHECK(Unwrap(listing.at(L"Docs/report.txt").logical_size) ==
        NtfsBrowserTests::mft_tree_report_data_size);
  CHECK(Unwrap(listing.at(L"Docs/report.txt").read_only));
}

TEMPLATE_TEST_CASE_SIG(
    "WalkMftTree from a directory lists only its subtree, "
    "by bare names",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenVolume<S>(BuildFakeNtfsImageWithMftTree());
  const MftTree tree(*volume);

  const Listing listing =
      WalkMftTree(tree, NtfsBrowserTests::mft_tree_docs_idx);

  CHECK(listing.size() == 2);
  CHECK(listing.contains(L"report.txt"));
  CHECK(listing.contains(L"link-b"));
}

TEMPLATE_TEST_CASE_SIG(
    "WalkMftTree on the dump tree lists the live entries "
    "only",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();
  const MftTree tree(*volume);

  const Listing listing = WalkMftTree(tree, root_record);

  // The freed "old.tmp" and "OldDir" and the zero-filled "Broken" are left
  // out, unlike in the index walk above.
  CHECK(listing.size() == 5);
  for (const std::wstring_view key :
       {L"Docs", L"Docs/report.txt", L"Docs/link-b", L"link-a", L"NewDir"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
  REQUIRE(listing.contains(L"Docs/report.txt"));
  CHECK(Unwrap(listing.at(L"Docs/report.txt").logical_size) ==
        NtfsBrowserTests::dump_report_content.size());
}
