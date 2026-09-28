#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "corpus-test-support.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// DFTT test #10 ("NTFS Autodetect", http://dftt.sourceforge.net): each
// partition image holds a valid NTFS filesystem, plus a second, unrelated
// one (Ext2, UFS2 or UFS1) formatted over it afterwards. Both remain
// mountable.
const std::filesystem::path kAutodetectDir =
    NtfsBrowserTests::kDfttDir / "10-ntfs-autodetect";

// Opens a DFTT autodetect partition image and confirms the library reads its
// NTFS side correctly: the root directory's own ntfs.txt is found, and its
// $DATA attribute reads back the size the index entry advertises.
void CheckReadsPartitionImage(std::wstring_view imageName)
{
  const std::filesystem::path imagePath = kAutodetectDir / imageName;
  NtfsBrowserTests::RequireCorpusImage(imagePath);

  NtfsVolume<Strategy::NO_CACHE> volume(
      NtfsBrowserTests::OpenBareVolumeImage(imagePath));
  REQUIRE(volume.IsVolumeOK());

  FileRecord root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> entry = root.FindSubEntry(L"ntfs.txt");
  REQUIRE(entry.has_value());
  CHECK_FALSE(entry->IsDirectory());
  CHECK(entry->GetFileSize() > 0);

  FileRecord file(volume);
  file.SetAttrMask(Mask::DATA);
  REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
  REQUIRE(file.ParseAttrs());

  const AttrBase<Strategy::NO_CACHE>* data = file.FindStream({});
  REQUIRE(data != nullptr);
  CHECK(data->GetDataSize() == entry->GetFileSize());
}

}  // namespace

TEST_CASE("Reads the NTFS side of DFTT test #10 partition 1 (NTFS+Ext2)",
          "[dftt][integration]")
{
  CheckReadsPartitionImage(L"10-ntfs-part1.dd");
}

TEST_CASE("Reads the NTFS side of DFTT test #10 partition 2 (NTFS+UFS2)",
          "[dftt][integration]")
{
  CheckReadsPartitionImage(L"10-ntfs-part2.dd");
}

TEST_CASE("Reads the NTFS side of DFTT test #10 partition 3 (NTFS+UFS1)",
          "[dftt][integration]")
{
  CheckReadsPartitionImage(L"10-ntfs-part3.dd");
}
