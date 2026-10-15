#include <ntfs-browser/win-types.h>

#include <optional>
#include <string_view>
#include <tuple>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr/file-name.h"
#include "attr/resident.h"
#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "ntfs-samples-support.h"
#include "optional-access.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::Attr::AttrFileName;
using NTFS_BROWSER_NAMESPACE::Attr::AttrResidentNoCache;

TEST_CASE(
    "STANDARD_INFORMATION and $I30 FILE_NAME disagree on /test/1.txt's last "
    "access time (ntfs-lastaccess.raw)",
    "[ntfs-samples][integration][timestamps]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(lastaccess_image,
                                                    "ntfs-lastaccess");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          lastaccess_image, small_image_partition_offset, "ntfs-lastaccess"));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"1.txt");
  REQUIRE(entry.has_value());

  FILETIME index_access{};
  NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileTime(nullptr, nullptr,
                                                           &index_access);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> file(volume);
  file.SetAttrMask(Attr::Mask::StandardInformation);
  REQUIRE(file.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  FILETIME std_info_access{};
  file.GetFileTime(nullptr, nullptr, &std_info_access);

  // ReadMe.md: 2019-03-03 12:37:55 ($STANDARD_INFORMATION) vs.
  // 2019-03-03 12:35:17 ($I30 FILE_NAME) - 2 min 38 s apart.
  CheckDeltaMatchesSeconds(TickDelta(std_info_access, index_access),
                           lastaccess_delta_seconds);

  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "STANDARD_INFORMATION, FILE_NAME and $I30 FILE_NAME each carry a "
    "different creation date for /test/test.txt (ntfs-si-vs-fn.raw)",
    "[ntfs-samples][integration][timestamps]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(si_vs_fn_image,
                                                    "ntfs-si-vs-fn");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          si_vs_fn_image, small_image_partition_offset, "ntfs-si-vs-fn"));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"test.txt");
  REQUIRE(entry.has_value());

  FILETIME index_create{};
  NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileTime(
      nullptr, &index_create, nullptr);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> file(volume);
  file.SetAttrMask(Attr::Mask::StandardInformation | Attr::Mask::FileName);
  REQUIRE(file.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  FILETIME std_info_create{};
  file.GetFileTime(nullptr, &std_info_create, nullptr);

  // The file record's own $FILE_NAME attribute: not the parent directory's
  // $I30 copy above, but the (possibly stale) one alongside this file's own
  // $STANDARD_INFORMATION. AttrFileName<>::GetFileTime() is the same,
  // otherwise-unreachable Filename::GetFileTime() IndexEntry uses; matches
  // FileRecord::GetFileTime()'s own internal cast (src/io/file-record.cpp).
  const auto& file_name_attrs = file.GetAttr(Attr::Type::FileName);
  REQUIRE_FALSE(file_name_attrs.empty());
  const auto* own_file_name = reinterpret_cast<
      const AttrFileName<AttrResidentNoCache, Cache::Strategy::NoCache>*>(
      file_name_attrs.front().get());
  FILETIME own_file_name_create{};
  own_file_name->GetFileTime(nullptr, &own_file_name_create, nullptr);

  // ReadMe.md: the file record's own $FILE_NAME says 2014-12-12, its
  // $STANDARD_INFORMATION says 2015-11-03, and the parent directory's $I30
  // $FILE_NAME says 2016-09-24.
  CHECK(ToDate(own_file_name_create) ==
        std::tuple<WORD, WORD, WORD>{2014, 12, 12});
  CHECK(ToDate(std_info_create) == std::tuple<WORD, WORD, WORD>{2015, 11, 3});
  CHECK(ToDate(index_create) == std::tuple<WORD, WORD, WORD>{2016, 9, 24});
}

TEST_CASE(
    "STANDARD_INFORMATION and $I30 FILE_NAME disagree on two files' last "
    "access time (ntfs.raw)",
    "[ntfs-samples][integration][timestamps]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireUnfetchedCorpusImage(ntfs_image,
                                                             "ntfs");

  // This image also documents two VSS shadow copies (one hidden from
  // "vssadmin list shadows") and a stray $I30 entry surviving in $MFT record
  // slack space. Neither is checked here: this library has no VSS support,
  // and ParseFileRecord()/ParseAttrs() only ever read a record's real,
  // in-use attribute area, never its unused slack bytes.
  NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          ntfs_image, large_image_partition_offset, "ntfs"));
  REQUIRE(volume.IsVolumeOK());

  const auto check_access_time_mismatch =
      [&volume](std::wstring_view dir_name, std::wstring_view file_name,
                ULONGLONG expected_delta_seconds) {
        NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> dir(
            volume);
        OpenDirPath(dir, {dir_name});
        const std::optional<IndexEntry> entry = dir.FindSubEntry(file_name);
        REQUIRE(entry.has_value());

        FILETIME index_access{};
        entry->GetFileTime(nullptr, nullptr, &index_access);

        NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> file(
            volume);
        file.SetAttrMask(Attr::Mask::StandardInformation);
        REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
        REQUIRE(file.ParseAttrs());

        FILETIME std_info_access{};
        file.GetFileTime(nullptr, nullptr, &std_info_access);

        CheckDeltaMatchesSeconds(TickDelta(std_info_access, index_access),
                                 expected_delta_seconds);
      };

  // ReadMe.md: 2020-07-25 12:49:21 vs. 12:35:48 - 13 min 33 s apart.
  check_access_time_mismatch(L"test_dir_2", L"file_2_1.txt",
                             long_mismatch_seconds);
  // ReadMe.md: 2020-07-25 12:33:24 vs. 12:35:24 - 2 min apart.
  check_access_time_mismatch(L"test_dir", L"file_1.txt",
                             short_mismatch_seconds);
}
