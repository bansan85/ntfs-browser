#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "md5-test-support.h"
#include "nps-ntfs1-test-support.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;

namespace
{

// Reads dirName's copy of every kKnownFiles entry and checks it against its
// ground truth size and MD5 (from ntfs1-gen2.xml). Both RAW's own on-disk
// bytes and Compressed's LZNT1-decompressed ones are expected to match it
// directly: Encrypted needs a decryption step first, covered separately in
// nps-ntfs1-efs-tests.cpp.
void CheckDirMatchesGroundTruth(const NtfsVolume<Strategy::NO_CACHE>& volume,
                                std::string_view dirName)
{
  FileRecord<Strategy::NO_CACHE> dir(volume);
  NtfsBrowserTests::OpenRootDir(dir);
  NtfsBrowserTests::OpenSubDir(dir, dirName);

  for (const NtfsBrowserTests::KnownFile& file : NtfsBrowserTests::kKnownFiles)
  {
    INFO(dirName << "/" << file.name);
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
          "[nps][integration]")
{
  NtfsBrowserTests::RequireCorpusImage(NtfsBrowserTests::kNtfs1Image);

  const NtfsVolume<Strategy::NO_CACHE> volume(
      NtfsBrowserTests::kNtfs1Image.wstring(), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  CheckDirMatchesGroundTruth(volume, "RAW");
}

TEST_CASE(
    "Compressed files decompress to the RAW ground truth (NPS ntfs1, gen2)",
    "[nps][integration]")
{
#ifndef NTFS_BROWSER_ENABLE_DECOMPRESSION
  SKIP(
      "decompression is not compiled in: a compressed attribute is "
      "rejected on sight (see attr-non-resident.cpp), so there is no "
      "meaningful ground-truth comparison left to make");
#endif

  NtfsBrowserTests::RequireCorpusImage(NtfsBrowserTests::kNtfs1Image);

  const NtfsVolume<Strategy::NO_CACHE> volume(
      NtfsBrowserTests::kNtfs1Image.wstring(), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  CheckDirMatchesGroundTruth(volume, "Compressed");
}
