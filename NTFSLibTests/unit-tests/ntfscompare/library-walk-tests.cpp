#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "compare-engine.h"
#include "entry.h"
#include "fake-ntfs-image.h"
#include "library-walk.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/win-types.h"
#include "optional-access.h"
#include "volume-open.h"

namespace NTFS_BROWSER_NAMESPACE {

struct VolumeOptions;

}  // namespace NTFS_BROWSER_NAMESPACE

#ifndef _WIN32
  #include <fstream>

  #include "os-api-walk.h"
  #include "std-filesystem-walk.h"
  #include "temp-dir.h"
#endif

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Cache::Strategy;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithDumpTree;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree;
using NTFS_BROWSER_NAMESPACE::Tests::FakeDataRun;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::Unwrap;
using NtfsCompare::CompareAgainstReference;
using NtfsCompare::CompareLibraryMethods;
using NtfsCompare::comparison_volume_options;
using NtfsCompare::Entry;
using NtfsCompare::Finding;
using NtfsCompare::Listing;
using NtfsCompare::Report;
using NtfsCompare::ResolveDirectoryRecord;
using NtfsCompare::WalkLibraryIndex;
using NtfsCompare::WalkMftTree;

namespace {

// Opens a fake image as a volume with cache strategy S and the given options.
template <Strategy S>
std::unique_ptr<NtfsVolume<S>>
    OpenVolume(std::vector<BYTE> image,
               const VolumeOptions& options = comparison_volume_options) {
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
std::unique_ptr<NtfsVolume<S>> OpenIndexedVolume(
    const VolumeOptions& options = comparison_volume_options) {
  const std::vector<FakeDataRun> big_runs{
      {.lcn = NTFS_BROWSER_NAMESPACE::Tests::dump_big_file_lcn,
       .clusters = NTFS_BROWSER_NAMESPACE::Tests::dump_big_file_clusters}};
  return OpenVolume<S>(
      BuildFakeNtfsImageWithDumpTree(
          NTFS_BROWSER_NAMESPACE::Tests::dump_big_file_size, big_runs),
      options);
}

// Looks name up in Docs (16) through FileRecord::FindSubEntry().
template <Strategy S>
std::optional<IndexEntry> FindInDocs(NtfsVolume<S>& volume,
                                     std::wstring_view name) {
  FileRecord<S> docs(volume);
  docs.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(
      docs.ParseFileRecord(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx));
  REQUIRE(docs.ParseAttrs());
  return docs.FindSubEntry(name);
}

// The record number of the volume's root directory.
constexpr ULONGLONG root_record =
    static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::Root);

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "ResolveDirectoryRecord finds the record of a directory "
    "path",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>();

  CHECK(ResolveDirectoryRecord(*volume, L"") == root_record);
  CHECK(ResolveDirectoryRecord(*volume, L"Docs") ==
        NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);
  CHECK(ResolveDirectoryRecord(*volume, L"Docs/") ==
        NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);
  CHECK(ResolveDirectoryRecord(*volume, L"/Docs") ==
        NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);
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

  const Listing listing = WalkLibraryIndex(
      *volume, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);

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
  CHECK(Unwrap(broken.logical_size) ==
        NTFS_BROWSER_NAMESPACE::Tests::dump_broken_real_size);
  CHECK(Unwrap(broken.physical_size) ==
        NTFS_BROWSER_NAMESPACE::Tests::dump_broken_alloc_size);
  CHECK(Unwrap(broken.modification_time_utc) ==
        NTFS_BROWSER_NAMESPACE::Tests::dump_broken_alter_time);
}

TEMPLATE_TEST_CASE_SIG(
    "WalkLibraryIndex from a directory lists a damaged entry only with "
    "recover_errors",
    "[ntfscompare][library-walk][characterization]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache) {
  const auto volume = OpenIndexedVolume<S>({.recover_errors = true});

  const Listing listing = WalkLibraryIndex(
      *volume, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);

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
        NTFS_BROWSER_NAMESPACE::Tests::mft_tree_deleted_file_idx);
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
  CHECK(found->GetFileReference() ==
        NTFS_BROWSER_NAMESPACE::Tests::mft_tree_zeroed_idx);
  CHECK(found->GetFileSize() ==
        NTFS_BROWSER_NAMESPACE::Tests::dump_broken_real_size);
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
        NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_data_size);
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
      WalkMftTree(tree, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);

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
  // out. Unlike the index walk, this one also reaches "link-a", "NewDir" and
  // "Docs/link-b", which no directory index names.
  CHECK(listing.size() == 5);
  for (const std::wstring_view key :
       {L"Docs", L"Docs/report.txt", L"Docs/link-b", L"link-a", L"NewDir"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
  REQUIRE(listing.contains(L"Docs/report.txt"));
  CHECK(Unwrap(listing.at(L"Docs/report.txt").logical_size) ==
        NTFS_BROWSER_NAMESPACE::Tests::dump_report_content.size());
}

TEST_CASE(
    "CompareLibraryMethods on the dump tree marks the live files no index "
    "names",
    "[ntfscompare][compare][characterization]") {
  const auto full_cache = OpenIndexedVolume<Strategy::FullCache>();
  const auto no_cache = OpenIndexedVolume<Strategy::NoCache>();
  const MftTree tree(*no_cache);

  const Listing full_listing = WalkLibraryIndex(*full_cache, root_record);
  const Listing no_listing = WalkLibraryIndex(*no_cache, root_record);
  const Listing tree_listing = WalkMftTree(tree, root_record);

  Report report;
  const Listing reference =
      CompareLibraryMethods(full_listing, no_listing, tree_listing, report);

  const auto count_kind = [&report](std::string_view kind) {
    return std::ranges::count_if(
        report.findings,
        [kind](const Finding& finding) { return finding.kind == kind; });
  };

  CHECK(reference.size() == 5);
  CHECK(report.unindexed_paths ==
        std::set<std::wstring>{L"Docs/link-b", L"NewDir", L"link-a"});
  CHECK(count_kind("UNINDEXED") == 3);
  CHECK(count_kind("LIB-MISSING") == 0);

  // Index entries and records disagree on the sizes and times of Docs and
  // report.txt. That is how NTFS keeps them, not a library mismatch: the
  // reference takes the record's values. Docs/Broken is in no listing, so it
  // is never counted.
  CHECK(count_kind("LIB-MISMATCH") == 0);
  const Entry& report_record = tree_listing.at(L"Docs/report.txt");
  const Entry& report_index = full_listing.at(L"Docs/report.txt");
  REQUIRE(report_record.logical_size != report_index.logical_size);
  CHECK(reference.at(L"Docs/report.txt").logical_size ==
        report_record.logical_size);

  CompareAgainstReference("std::filesystem", reference, full_listing, report);

  REQUIRE(report.stats.size() == 2);
  CHECK(report.stats.back().compared_entries == 2);
  CHECK(report.stats.back().missing == 0);
  CHECK(report.stats.back().extra == 0);
  CHECK(report.stats.back().unindexed == 3);
  CHECK(count_kind("MISSING") == 0);
  CHECK(count_kind("EXTRA") == 0);
}

#ifndef _WIN32

namespace {

// The keys of listing, in order.
std::vector<std::wstring> Keys(const Listing& listing) {
  std::vector<std::wstring> keys;
  for (const auto& listed : listing) {
    keys.push_back(listed.first);
  }
  return keys;
}

}  // namespace

TEST_CASE("A name with an unpaired surrogate is the same in every method",
          "[ntfscompare][library-walk][names]") {
  // The NTFS name U+D800, "a" has no UTF-8 form: ntfs-3g shows it as the
  // WTF-8 bytes ED A0 80 61.
  const std::wstring name = {static_cast<wchar_t>(0xD800), L'a'};
  const NTFS_BROWSER_NAMESPACE::Tests::TempDir dir;
  const std::string os_name =
      "\xED\xA0\x80"
      "a";
  {
    std::error_code ignored;
    std::ofstream file(dir.Path() / os_name, std::ios::binary);
    if (!file) {
      SKIP("the temp file system refuses a name that is not valid UTF-8");
    }
  }

  const auto volume = OpenVolume<Strategy::NoCache>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithReportName(name));
  const auto docs = ResolveDirectoryRecord(*volume, L"Docs");
  REQUIRE(docs.has_value());
  const Listing library = WalkLibraryIndex(*volume, *docs);

  const std::vector<std::wstring> expected{name};
  CHECK(Keys(library) == expected);
  CHECK(Keys(NtfsCompare::WalkStdFilesystem(dir.Path())) == expected);
  CHECK(Keys(NtfsCompare::WalkOsApi(dir.Path())) == expected);
}

TEST_CASE("An accented name is the same in every method",
          "[ntfscompare][library-walk][names]") {
  // The image stores UTF-16, so the character outside the BMP is a pair of
  // code units there, and one wchar_t in the listings.
  const std::wstring stored = std::wstring(L"café-") +
                              static_cast<wchar_t>(0xD83D) +
                              static_cast<wchar_t>(0xDE00) + L".txt";
  const std::wstring name = L"café-\U0001F600.txt";
  const NTFS_BROWSER_NAMESPACE::Tests::TempDir dir;
  const std::filesystem::path os_file = dir.Write(
      "caf\xC3\xA9-\xF0\x9F\x98\x80"
      ".txt",
      "x");
  REQUIRE(std::filesystem::exists(os_file));

  const auto volume = OpenVolume<Strategy::NoCache>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithReportName(stored));
  const auto docs = ResolveDirectoryRecord(*volume, L"Docs");
  REQUIRE(docs.has_value());

  const std::vector<std::wstring> expected{name};
  CHECK(Keys(WalkLibraryIndex(*volume, *docs)) == expected);
  CHECK(Keys(NtfsCompare::WalkStdFilesystem(dir.Path())) == expected);
  CHECK(Keys(NtfsCompare::WalkOsApi(dir.Path())) == expected);
}

#endif  // !_WIN32
