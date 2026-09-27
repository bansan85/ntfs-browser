#pragma once

#include <filesystem>

namespace NtfsBrowserTests
{

// Roots of the forensic image corpora, outside this repo. CMake sets each one
// from its NTFS_BROWSER_TEST_* cache or environment variable, as a UTF-8 path.
// .github/scripts/fetch-test-data.py builds the default layout.

// DFTT (dftt.sourceforge.net): one subdirectory per unzipped test.
inline const std::filesystem::path kDfttDir = u8"" NTFS_TEST_DFTT_DIR;
// Digital Corpora nps-2009-ntfs1: ntfs1-gen2.raw, converted from its .E01.
inline const std::filesystem::path kNpsNtfs1Dir = u8"" NTFS_TEST_NPS_NTFS1_DIR;
// github.com/msuhanov/ntfs-samples, each *.raw.gz decompressed in place.
inline const std::filesystem::path kNtfsSamplesDir =
    u8"" NTFS_TEST_NTFS_SAMPLES_DIR;
// ntfs_extremely_fragmented_mft.raw: 256 GiB, so it may live elsewhere.
inline const std::filesystem::path kFragmentedMftImage =
    u8"" NTFS_TEST_FRAGMENTED_MFT_IMAGE;

// SKIPs the running test when image is absent. Under
// NTFS_BROWSER_REQUIRE_TEST_DATA, FAILs it instead: CI fetches every image
// this guards, so a skip there would hide a broken fetch.
void RequireCorpusImage(const std::filesystem::path& image);

}  // namespace NtfsBrowserTests
