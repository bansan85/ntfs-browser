#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "corpus-test-support.h"
#include "optional-access.h"

using NtfsBrowser::IndexEntry;
using NtfsBrowser::Attr::AttrBase;
using NtfsBrowser::Io::FileRecord;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;

namespace {

// DFTT test #10 ("NTFS Autodetect", http://dftt.sourceforge.net): each
// partition image holds a valid NTFS filesystem, plus a second, unrelated
// one (Ext2, UFS2 or UFS1) formatted over it afterwards. Both remain
// mountable.
const std::filesystem::path autodetect_dir =
    NtfsBrowserTests::DfttDir() / "10-ntfs-autodetect";

// Opens a DFTT autodetect partition image and confirms the library reads its
// NTFS side correctly: the root directory's own ntfs.txt is found, and its
// $DATA attribute reads back the size the index entry advertises.
void CheckReadsPartitionImage(std::wstring_view image_name) {
  const std::filesystem::path image_path = autodetect_dir / image_name;
  NtfsBrowserTests::RequireCorpusImage(image_path);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NtfsBrowserTests::OpenBareVolumeImage(image_path));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> entry = root.FindSubEntry(L"ntfs.txt");
  REQUIRE(entry.has_value());
  CHECK_FALSE(NtfsBrowserTests::Unwrap(entry).IsDirectory());
  CHECK(NtfsBrowserTests::Unwrap(entry).GetFileSize() > 0);

  NtfsBrowser::Io::FileRecord file(volume);
  file.SetAttrMask(Attr::Mask::Data);
  REQUIRE(
      file.ParseFileRecord(NtfsBrowserTests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  const NtfsBrowser::Attr::AttrBase<Cache::Strategy::NoCache>* data =
      file.FindStream({});
  REQUIRE(data != nullptr);
  CHECK(data->GetDataSize() == NtfsBrowserTests::Unwrap(entry).GetFileSize());
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
