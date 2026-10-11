#include <ntfs-browser/win-types.h>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "md5-test-support.h"
#include "nps-ntfs1-test-support.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;

namespace {

// Reads dirName's copy of every known_files entry and checks it against its
// ground truth size and MD5 (from ntfs1-gen2.xml). Both RAW's own on-disk
// bytes and Compressed's LZNT1-decompressed ones are expected to match it
// directly: Encrypted needs a decryption step first, covered separately in
// nps-ntfs1-efs-tests.cpp.
void CheckDirMatchesGroundTruth(
    const NtfsVolume<Cache::Strategy::NoCache>& volume,
    std::string_view dir_name) {
  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> dir(volume);
  NtfsBrowserTests::OpenRootDir(dir);
  NtfsBrowserTests::OpenSubDir(dir, dir_name);

  for (const NtfsBrowserTests::KnownFile& file :
       NtfsBrowserTests::known_files) {
    INFO(dir_name << "/" << file.name);
    const std::vector<BYTE> data =
        NtfsBrowserTests::ReadFile(volume, dir, file.name);
    CHECK(data.size() == file.size);
#ifdef NTFS_TEST_HAS_MD5
    CHECK(NtfsBrowserTests::Md5Hex(data) == file.md5);
#endif
  }
}

}  // namespace

TEST_CASE("RAW files recover byte-for-byte from the NPS ntfs1 corpus (gen2)",
          "[nps][integration]") {
  NtfsBrowserTests::RequireCorpusImage(NtfsBrowserTests::ntfs1_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NtfsBrowserTests::OpenNtfs1Image(), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  CheckDirMatchesGroundTruth(volume, "RAW");
}

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
TEST_CASE(
    "Compressed files decompress to the RAW ground truth (NPS ntfs1, gen2)",
    "[nps][integration]") {
  NtfsBrowserTests::RequireCorpusImage(NtfsBrowserTests::ntfs1_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NtfsBrowserTests::OpenNtfs1Image(), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  CheckDirMatchesGroundTruth(volume, "Compressed");
}
#endif
