#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "corpus-test-support.h"
#include "md5-test-support.h"
#include "optional-access.h"

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::Attr::AttrBase;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

// DFTT test #10 ("NTFS Autodetect", http://dftt.sourceforge.net): each
// partition image holds a valid NTFS filesystem, plus a second, unrelated
// one (Ext2, UFS2 or UFS1) formatted over it afterwards. Both remain
// mountable.
const std::filesystem::path autodetect_dir =
    NTFS_BROWSER_NAMESPACE::Tests::DfttDir() / "10-ntfs-autodetect";

// Opens a DFTT autodetect partition image and confirms the library reads its
// NTFS side correctly: the root directory's own ntfs.txt is found, and its
// $DATA attribute reads back the size the index entry advertises.
void CheckReadsPartitionImage(std::wstring_view image_name) {
  const std::filesystem::path image_path = autodetect_dir / image_name;
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(image_path);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenBareVolumeImage(image_path));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> entry = root.FindSubEntry(L"ntfs.txt");
  REQUIRE(entry.has_value());
  CHECK_FALSE(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).IsDirectory());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileSize() > 0);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord file(volume);
  file.SetAttrMask(Attr::Mask::Data);
  REQUIRE(file.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<Cache::Strategy::NoCache>* data =
      file.FindStream({});
  REQUIRE(data != nullptr);
  CHECK(data->GetDataSize() ==
        NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileSize());
}

}  // namespace

TEST_CASE("Reads the NTFS side of DFTT test #10 partition 1 (NTFS+Ext2)",
          "[dftt][integration]") {
  CheckReadsPartitionImage(L"10-ntfs-part1.dd");
}

TEST_CASE("Reads the NTFS side of DFTT test #10 partition 2 (NTFS+UFS2)",
          "[dftt][integration]") {
  CheckReadsPartitionImage(L"10-ntfs-part2.dd");
}

TEST_CASE("Reads the NTFS side of DFTT test #10 partition 3 (NTFS+UFS1)",
          "[dftt][integration]") {
  CheckReadsPartitionImage(L"10-ntfs-part3.dd");
}

using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// DFTT test #3 ("NTFS Keyword Search #1", http://dftt.sf.net): an 8 MB NTFS
// file system holding ten ASCII search terms, each placed to exercise one
// combination of resident/non-resident, allocated/deleted, and plain/
// alternate-data-stream content.
const std::filesystem::path dftt_kwsrch_image =
    NTFS_BROWSER_NAMESPACE::Tests::DfttDir() / "3-kwsrch-ntfs" /
    "ntfs-img-kw-1.dd";

// One DFTT test #3 search-term case, addressed by its own MFT record number
// instead of by path (index.html gives the search term and the file it
// lives in; the record numbers themselves come from walking the image).
struct KeywordFile {
  ULONGLONG mft_record;
  std::wstring_view stream_name;  // {} for the unnamed $DATA stream
  std::string_view keyword;
  ULONGLONG size;
  bool deleted;
  bool is_directory;
};

// Sizes index.html gives the files: resident, non-resident, and non-resident
// across fragmented clusters.
constexpr ULONGLONG resident_size = 120;
constexpr ULONGLONG non_resident_size = 2000;
constexpr ULONGLONG fragmented_size = 2600;

// The MFT record of each search-term case, from walking the image.
constexpr KeywordFile resident_alloc{27,    {},   "r-alloc", resident_size,
                                     false, false};
constexpr KeywordFile resident_file_ads{29,    L"here", "r-fads", resident_size,
                                        false, false};
constexpr KeywordFile resident_dir_ads{30,    L"there", "r-dads", resident_size,
                                       false, true};
constexpr KeywordFile non_resident_alloc{
    33, {}, "n-alloc", non_resident_size, false, false};
constexpr KeywordFile non_resident_frag{35,    {},   "n-frag", fragmented_size,
                                        false, false};
constexpr KeywordFile non_resident_file_ads{
    37, L"here", "n-fads", non_resident_size, false, false};
constexpr KeywordFile non_resident_dir_ads{
    38, L"there", "n-dads", non_resident_size, false, true};
constexpr KeywordFile resident_unalloc{34,   {},   "r-unalloc", resident_size,
                                       true, false};

// The non-resident file whose search term sits in its slack space.
constexpr ULONGLONG slack_record = 36;

// Parses one MFT record from `volume` and checks that the requested stream
// has the size and deletion/directory state index.html documents, and that
// its content contains the DFTT search term.
void CheckReadsKeywordFile(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                           const KeywordFile& file) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord record(volume);
  record.SetAttrMask(Attr::Mask::Data);
  REQUIRE(record.ParseFileRecord(file.mft_record));
  CHECK(record.IsDeleted() == file.deleted);
  REQUIRE(record.ParseAttrs());
  CHECK(record.IsDirectory() == file.is_directory);

  const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<Cache::Strategy::NoCache>*
      stream = record.FindStream(file.stream_name);
  REQUIRE(stream != nullptr);
  REQUIRE(stream->GetDataSize() == file.size);

  std::vector<BYTE> data(file.size);
  REQUIRE(stream->ReadData(0, data) == file.size);
  const std::string_view content(reinterpret_cast<const char*>(data.data()),
                                 data.size());
  CHECK_THAT(std::string(content),
             Catch::Matchers::ContainsSubstring(std::string(file.keyword)));
}

}  // namespace

TEST_CASE("Reads DFTT test #3 (NTFS Keyword Search) files",
          "[dftt][integration]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(dftt_kwsrch_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenBareVolumeImage(dftt_kwsrch_image));
  REQUIRE(volume.IsVolumeOK());

  CheckReadsKeywordFile(volume, resident_alloc);
  CheckReadsKeywordFile(volume, resident_file_ads);
  CheckReadsKeywordFile(volume, resident_dir_ads);
  CheckReadsKeywordFile(volume, non_resident_alloc);
  CheckReadsKeywordFile(volume, non_resident_frag);
  CheckReadsKeywordFile(volume, non_resident_file_ads);
  CheckReadsKeywordFile(volume, non_resident_dir_ads);

  // Resident unallocated (deleted) file: ParseAttrs() only sees it with
  // include_deleted on.
  VolumeOptions options;
  options.include_deleted = true;
  const NtfsVolume<Cache::Strategy::NoCache> del_volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenBareVolumeImage(dftt_kwsrch_image),
      options);
  REQUIRE(del_volume.IsVolumeOK());
  CheckReadsKeywordFile(del_volume, resident_unalloc);

  // Non-resident allocated file: index.html places its search term in this
  // file's slack space, past its logical size. ReadData()/GetDataSize()
  // expose only the file's own content, so the term MUST NOT be found there.
  NTFS_BROWSER_NAMESPACE::Io::FileRecord slack(volume);
  slack.SetAttrMask(Attr::Mask::Data);
  REQUIRE(slack.ParseFileRecord(slack_record));
  REQUIRE(slack.ParseAttrs());
  const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<Cache::Strategy::NoCache>*
      slack_data = slack.FindStream({});
  REQUIRE(slack_data != nullptr);
  REQUIRE(slack_data->GetDataSize() == non_resident_size);
  std::vector<BYTE> data(non_resident_size);
  REQUIRE(slack_data->ReadData(0, data) == non_resident_size);
  const std::string_view content(reinterpret_cast<const char*>(data.data()),
                                 data.size());
  CHECK_THAT(std::string(content),
             !Catch::Matchers::ContainsSubstring("n-slack"));
}

#ifdef NTFS_TEST_HAS_MD5
using NTFS_BROWSER_NAMESPACE::Tests::Md5Hex;
#endif

namespace {

// DFTT test #7 ("NTFS Undelete", http://dftt.sf.net): a 6 MB NTFS file
// system with eight deleted files, two deleted directories, and a deleted
// alternate data stream, none of which were touched afterwards.
const std::filesystem::path dftt_undel_image =
    NTFS_BROWSER_NAMESPACE::Tests::DfttDir() / "7-undel-ntfs" /
    "7-ntfs-undel.dd";

// One DFTT test #7 file, addressed directly by its own MFT record number
// instead of by path: index.html documents that dir3, the parent of
// sing2.dat, has itself been deleted and its record reallocated (to
// res1.dat), so a name-based lookup from the root down would not find it.
struct DeletedFile {
  ULONGLONG mft_record;
  std::wstring_view stream_name;  // {} for the unnamed $DATA stream
  ULONGLONG size;
  std::string_view md5;
};

// Facts from the image's index.html, per file. The MD5s are skipped without
// NTFS_TEST_HAS_MD5.
constexpr DeletedFile resident_file{
    37, {}, 101, "9036637712b491904cd0bfbdbe648453"};
constexpr DeletedFile single_cluster_file{
    31, {}, 780, "59b20779f69ff9f0ac5fcd2c38835a79"};
// A multiple cluster, non-fragmented file, and its named ADS: same record.
constexpr DeletedFile multi_cluster_file{
    32, {}, 3801, "ffd27bd782bdce67750b6b9ee069d2ef"};
constexpr DeletedFile multi_cluster_ads{32, L"ADS", 1234,
                                        "ba1b9eedb1c091ddca253d35dde8f616"};
// Fragmented files, interleaved with each other on disk.
constexpr DeletedFile fragmented_file1{
    29, {}, 1584, "7a3bc5b763bef201202108f4ba128149"};
constexpr DeletedFile fragmented_file2{
    30, {}, 3873, "0e80ab84ef0087e60dfc67b88a1cf13e"};
// Files in deleted directories.
constexpr DeletedFile in_deleted_dir1_file{
    36, {}, 1715, "59cf0e9cd107bc1e75afb7374f6e05bb"};
constexpr DeletedFile in_deleted_dir2_file{
    35, {}, 2027, "21121699487f3fbbdb9a4b3391b6d3e0"};
// In a directory whose own MFT record has been reallocated.
constexpr DeletedFile in_reallocated_dir_file{
    38, {}, 1005, "c229626f6a71b167ad7e50c4f2fccdb1"};

// Recovers one deleted file by MFT record number and checks it against its
// known size and MD5 (from index.html, skipped without NTFS_TEST_HAS_MD5),
// plus the Feb 29, 2004 (leap year) creation date every file here shares.
void CheckRecoversDeletedFile(
    const NtfsVolume<Cache::Strategy::NoCache>& volume,
    const DeletedFile& file) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord record(volume);
  record.SetAttrMask(Attr::Mask::Data | Attr::Mask::StandardInformation);
  REQUIRE(record.ParseFileRecord(file.mft_record));
  CHECK(record.IsDeleted());
  REQUIRE(record.ParseAttrs());

  FILETIME create_time{};
  record.GetFileTime(nullptr, &create_time, nullptr);
  const auto [year, month, day] =
      NTFS_BROWSER_NAMESPACE::Tests::FileTimeToDate(create_time);
  CHECK(year == 2004);
  CHECK(month == 2);
  CHECK(day == 29);

  const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<Cache::Strategy::NoCache>*
      stream = record.FindStream(file.stream_name);
  REQUIRE(stream != nullptr);
  REQUIRE(stream->GetDataSize() == file.size);

  std::vector<BYTE> data(file.size);
  REQUIRE(stream->ReadData(0, data) == file.size);
#ifdef NTFS_TEST_HAS_MD5
  CHECK(Md5Hex(data) == file.md5);
#endif
}

}  // namespace

TEST_CASE("Recovers deleted files from DFTT test #7 (NTFS Undelete)",
          "[dftt][integration]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(dftt_undel_image);

  VolumeOptions options;
  options.include_deleted = true;
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenBareVolumeImage(dftt_undel_image),
      options);
  REQUIRE(volume.IsVolumeOK());

  CheckRecoversDeletedFile(volume, resident_file);
  CheckRecoversDeletedFile(volume, single_cluster_file);
  CheckRecoversDeletedFile(volume, multi_cluster_file);
  CheckRecoversDeletedFile(volume, multi_cluster_ads);
  CheckRecoversDeletedFile(volume, fragmented_file1);
  CheckRecoversDeletedFile(volume, fragmented_file2);
  CheckRecoversDeletedFile(volume, in_deleted_dir1_file);
  CheckRecoversDeletedFile(volume, in_deleted_dir2_file);
  CheckRecoversDeletedFile(volume, in_reallocated_dir_file);

  // The two deleted directories themselves.
  NTFS_BROWSER_NAMESPACE::Io::FileRecord dir1(volume);
  REQUIRE(dir1.ParseFileRecord(33));
  CHECK(dir1.IsDeleted());
  CHECK(dir1.IsDirectory());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord dir1_dir2(volume);
  REQUIRE(dir1_dir2.ParseFileRecord(34));
  CHECK(dir1_dir2.IsDeleted());
  CHECK(dir1_dir2.IsDirectory());
}
