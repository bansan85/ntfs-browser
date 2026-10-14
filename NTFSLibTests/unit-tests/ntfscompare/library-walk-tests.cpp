#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "entry.h"
#include "fake-ntfs-image.h"
#include "library-walk.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Cache::Strategy;
using NtfsBrowser::Io::FileRecord;
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

// Opens a fake image as a volume with cache strategy S and the given options.
template <Strategy S>
std::unique_ptr<NtfsVolume<S>> OpenVolume(std::vector<BYTE> image,
                                          const VolumeOptions& options = {}) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(std::move(image)), options);
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

// The dump-tree image, whose directories carry real index blocks. Its root
// (#5) indexes "big.bin" (6), "Docs" (16) and "empty.txt" (7). Docs (16)
// indexes "Broken" (25), "OldDir" (20, freed), "old.tmp" (19, freed) and
// "report.txt" (17).
template <Strategy S>
std::unique_ptr<NtfsVolume<S>>
    OpenIndexedVolume(const VolumeOptions& options = {}) {
  const std::vector<FakeDataRun> big_runs{
      {.lcn = NtfsBrowserTests::dump_big_file_lcn,
       .clusters = NtfsBrowserTests::dump_big_file_clusters}};
  return OpenVolume<S>(BuildFakeNtfsImageWithDumpTree(
                           NtfsBrowserTests::dump_big_file_size, big_runs),
                       options);
}

// Looks name up in Docs (16) through FileRecord::FindSubEntry().
template <Strategy S>
std::optional<IndexEntry> FindInDocs(NtfsVolume<S>& volume,
                                     std::wstring_view name) {
  FileRecord<S> docs(volume);
  docs.SetAttrMask(NtfsBrowser::Attr::Mask::IndexRoot |
                   NtfsBrowser::Attr::Mask::IndexAllocation);
  REQUIRE(docs.ParseFileRecord(NtfsBrowserTests::mft_tree_docs_idx));
  REQUIRE(docs.ParseAttrs());
  return docs.FindSubEntry(name);
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
    "WalkLibraryIndex skips freed entries below the root by default",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();

  const Listing listing = WalkLibraryIndex(*volume, root_record);

  // big.bin and empty.txt are below the first user record, so both are left
  // out as system metafiles. OldDir and old.tmp are freed, so they are hidden.
  // Broken does not parse, which is damage: strict mode hides it too.
  CHECK(listing.size() == 2);
  for (const std::wstring_view key : {L"Docs", L"Docs/report.txt"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
  CHECK_FALSE(listing.contains(L"Docs/Broken"));
  CHECK_FALSE(listing.contains(L"Docs/OldDir"));
  CHECK_FALSE(listing.contains(L"Docs/old.tmp"));
  REQUIRE(listing.contains(L"Docs"));
  CHECK(listing.at(L"Docs").is_directory);
  REQUIRE(listing.contains(L"Docs/report.txt"));
  CHECK_FALSE(listing.at(L"Docs/report.txt").is_directory);
  CHECK(listing.at(L"Docs/report.txt").physical_size.has_value());
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex lists freed entries below the root with "
    "include_deleted",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>({.include_deleted = true});

  const Listing listing = WalkLibraryIndex(*volume, root_record);

  // Broken is still damaged, so it stays hidden without recover_errors.
  CHECK(listing.size() == 4);
  for (const std::wstring_view key :
       {L"Docs", L"Docs/OldDir", L"Docs/old.tmp", L"Docs/report.txt"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
  CHECK_FALSE(listing.contains(L"Docs/Broken"));
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex from a directory lists only its "
    "live subtree, by bare names",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();

  const Listing listing =
      WalkLibraryIndex(*volume, NtfsBrowserTests::mft_tree_docs_idx);

  CHECK(listing.size() == 1);
  CHECK(listing.contains(L"report.txt"));
  CHECK_FALSE(listing.contains(L"Broken"));
  CHECK_FALSE(listing.contains(L"OldDir"));
  CHECK_FALSE(listing.contains(L"old.tmp"));
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex with recover_errors lists a damaged entry from its "
    "index metadata",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>({.recover_errors = true});

  const Listing listing = WalkLibraryIndex(*volume, root_record);

  // Freed entries stay hidden: include_deleted is still off.
  CHECK(listing.size() == 3);
  CHECK_FALSE(listing.contains(L"Docs/OldDir"));
  CHECK_FALSE(listing.contains(L"Docs/old.tmp"));
  REQUIRE(listing.contains(L"Docs/Broken"));
  const Entry& broken = listing.at(L"Docs/Broken");
  CHECK_FALSE(broken.is_directory);
  CHECK(Unwrap(broken.logical_size) == NtfsBrowserTests::dump_broken_real_size);
  CHECK(Unwrap(broken.physical_size) ==
        NtfsBrowserTests::dump_broken_alloc_size);
  CHECK(Unwrap(broken.modification_time_utc) ==
        NtfsBrowserTests::dump_broken_alter_time);
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex from a directory lists a damaged entry only with "
    "recover_errors",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>({.recover_errors = true});

  const Listing listing =
      WalkLibraryIndex(*volume, NtfsBrowserTests::mft_tree_docs_idx);

  CHECK(listing.size() == 2);
  CHECK(listing.contains(L"Broken"));
  CHECK(listing.contains(L"report.txt"));
  CHECK_FALSE(listing.contains(L"OldDir"));
  CHECK_FALSE(listing.contains(L"old.tmp"));
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex with include_deleted and recover_errors lists every "
    "index entry",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume =
      OpenIndexedVolume<S>({.include_deleted = true, .recover_errors = true});

  const Listing listing = WalkLibraryIndex(*volume, root_record);

  CHECK(listing.size() == 5);
  for (const std::wstring_view key : {L"Docs", L"Docs/Broken", L"Docs/OldDir",
                                      L"Docs/old.tmp", L"Docs/report.txt"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry in Docs hides a freed name unless include_deleted is set",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto hidden = OpenIndexedVolume<S>();
  CHECK_FALSE(FindInDocs(*hidden, L"old.tmp").has_value());
  CHECK(FindInDocs(*hidden, L"report.txt").has_value());

  const auto shown = OpenIndexedVolume<S>({.include_deleted = true});
  const std::optional<IndexEntry> found = FindInDocs(*shown, L"old.tmp");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() ==
        NtfsBrowserTests::mft_tree_deleted_file_idx);
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry in Docs returns a damaged name only under recover_errors",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto strict = OpenIndexedVolume<S>();
  CHECK_FALSE(FindInDocs(*strict, L"Broken").has_value());

  // include_deleted does not show a damaged name: only recover_errors does.
  const auto deleted_only = OpenIndexedVolume<S>({.include_deleted = true});
  CHECK_FALSE(FindInDocs(*deleted_only, L"Broken").has_value());

  const auto recovering = OpenIndexedVolume<S>({.recover_errors = true});
  const std::optional<IndexEntry> found = FindInDocs(*recovering, L"Broken");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == NtfsBrowserTests::mft_tree_zeroed_idx);
  CHECK(found->GetFileSize() == NtfsBrowserTests::dump_broken_real_size);
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
