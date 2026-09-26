#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "md5-test-support.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;
#ifdef NTFS_TEST_HAS_MD5
using NtfsBrowserTests::Md5Hex;
#endif

namespace
{

// DFTT test #7 ("NTFS Undelete", http://dftt.sf.net): a 6 MB NTFS file
// system with eight deleted files, two deleted directories, and a deleted
// alternate data stream, none of which were touched afterwards. Not part of
// this repo: hardcoded here for now.
const std::filesystem::path kDfttImage =
    LR"(H:\repos\ntfs-database\dftt\7-undel-ntfs\7-ntfs-undel.dd)";

// One DFTT test #7 file, addressed directly by its own MFT record number
// instead of by path: index.html documents that dir3, the parent of
// sing2.dat, has itself been deleted and its record reallocated (to
// res1.dat), so a name-based lookup from the root down would not find it.
struct DeletedFile
{
  ULONGLONG mft_record;
  std::wstring_view stream_name;  // {} for the unnamed $DATA stream
  ULONGLONG size;
  std::string_view md5;
};

// Recovers one deleted file by MFT record number and checks it against its
// known size and MD5 (from index.html, skipped without NTFS_TEST_HAS_MD5),
// plus the Feb 29, 2004 (leap year) creation date every file here shares.
void CheckRecoversDeletedFile(const NtfsVolume<Strategy::NO_CACHE>& volume,
                              const DeletedFile& file)
{
  FileRecord record(volume);
  record.SetAttrMask(Mask::DATA | Mask::STANDARD_INFORMATION);
  REQUIRE(record.ParseFileRecord(file.mft_record));
  CHECK(record.IsDeleted());
  REQUIRE(record.ParseAttrs());

  FILETIME create_time{};
  record.GetFileTime(nullptr, &create_time, nullptr);
  SYSTEMTIME create_st{};
  REQUIRE(FileTimeToSystemTime(&create_time, &create_st) == TRUE);
  CHECK(create_st.wYear == 2004);
  CHECK(create_st.wMonth == 2);
  CHECK(create_st.wDay == 29);

  const AttrBase<Strategy::NO_CACHE>* stream =
      record.FindStream(file.stream_name);
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
          "[dftt][integration]")
{
  if (!std::filesystem::exists(kDfttImage))
  {
    SKIP("DFTT test image not present: " << kDfttImage.string());
  }

  VolumeOptions options;
  options.include_deleted = true;
  NtfsVolume<Strategy::NO_CACHE> volume(kDfttImage.wstring(), options);
  REQUIRE(volume.IsVolumeOK());

  // Resident file.
  CheckRecoversDeletedFile(volume,
                           {37, {}, 101, "9036637712b491904cd0bfbdbe648453"});
  // Single cluster file.
  CheckRecoversDeletedFile(volume,
                           {31, {}, 780, "59b20779f69ff9f0ac5fcd2c38835a79"});
  // Multiple cluster, non-fragmented file, and its named ADS - same record.
  CheckRecoversDeletedFile(volume,
                           {32, {}, 3801, "ffd27bd782bdce67750b6b9ee069d2ef"});
  CheckRecoversDeletedFile(
      volume, {32, L"ADS", 1234, "ba1b9eedb1c091ddca253d35dde8f616"});
  // Fragmented files, interleaved with each other on disk.
  CheckRecoversDeletedFile(volume,
                           {29, {}, 1584, "7a3bc5b763bef201202108f4ba128149"});
  CheckRecoversDeletedFile(volume,
                           {30, {}, 3873, "0e80ab84ef0087e60dfc67b88a1cf13e"});
  // Files in deleted directories.
  CheckRecoversDeletedFile(volume,
                           {36, {}, 1715, "59cf0e9cd107bc1e75afb7374f6e05bb"});
  CheckRecoversDeletedFile(volume,
                           {35, {}, 2027, "21121699487f3fbbdb9a4b3391b6d3e0"});
  // In a directory whose own MFT record has been reallocated.
  CheckRecoversDeletedFile(volume,
                           {38, {}, 1005, "c229626f6a71b167ad7e50c4f2fccdb1"});

  // The two deleted directories themselves.
  FileRecord dir1(volume);
  REQUIRE(dir1.ParseFileRecord(33));
  CHECK(dir1.IsDeleted());
  CHECK(dir1.IsDirectory());

  FileRecord dir1_dir2(volume);
  REQUIRE(dir1_dir2.ParseFileRecord(34));
  CHECK(dir1_dir2.IsDeleted());
  CHECK(dir1_dir2.IsDirectory());
}
