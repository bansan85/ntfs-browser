#include <ntfs-browser/win-types.h>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "md5-test-support.h"
#include "nps-ntfs1-test-support.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// Reads dirName's copy of every known_files entry and checks it against its
// ground truth size and MD5 (from ntfs1-gen2.xml). Both RAW's own on-disk
// bytes and Compressed's LZNT1-decompressed ones are expected to match it
// directly: Encrypted needs a decryption step first, covered separately in
// nps-ntfs1-efs-tests.cpp.
void CheckDirMatchesGroundTruth(
    const NtfsVolume<Cache::Strategy::NoCache>& volume,
    std::string_view dir_name) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> dir(volume);
  NTFS_BROWSER_NAMESPACE::Tests::OpenRootDir(dir);
  NTFS_BROWSER_NAMESPACE::Tests::OpenSubDir(dir, dir_name);

  for (const NTFS_BROWSER_NAMESPACE::Tests::KnownFile& file :
       NTFS_BROWSER_NAMESPACE::Tests::known_files) {
    INFO(dir_name << "/" << file.name);
    const std::vector<BYTE> data =
        NTFS_BROWSER_NAMESPACE::Tests::ReadFile(volume, dir, file.name);
    CHECK(data.size() == file.size);
#ifdef NTFS_TEST_HAS_MD5
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::Md5Hex(data) == file.md5);
#endif
  }
}

}  // namespace

TEST_CASE("RAW files recover byte-for-byte from the NPS ntfs1 corpus (gen2)",
          "[nps][integration]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(
      NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image, "nps-ntfs1-raw");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image, 0, "nps-ntfs1-raw"),
      VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  CheckDirMatchesGroundTruth(volume, "RAW");
}

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
TEST_CASE(
    "Compressed files decompress to the RAW ground truth (NPS ntfs1, gen2)",
    "[nps][integration]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(
      NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image, "nps-ntfs1-compressed");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image, 0,
          "nps-ntfs1-compressed"),
      VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  CheckDirMatchesGroundTruth(volume, "Compressed");
}
#endif
