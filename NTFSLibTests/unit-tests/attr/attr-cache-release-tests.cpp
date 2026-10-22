#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "efs/fek.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/disk-reader.h"

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #include "efs-test-support.h"
  #include "efs-tests-support.h"
#endif

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION

namespace {

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using CompressedBuilder = std::vector<BYTE> (*)();

// Bytes of one FullCache block, and so the smallest ceiling that keeps one.
constexpr std::uint64_t release_block = 65536;

// Passes over the file at each ceiling. Each builds a new record, so no
// decompressed unit survives from the one before: every pass reads the disk.
constexpr int release_passes = 3;

// The whole $DATA stream of a fresh root record, read through a FullCache
// volume with the given ceiling.
std::vector<BYTE> ReadWholeCompressed(const std::vector<BYTE>& image,
                                      std::uint64_t ceiling) {
  const NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>
      volume(std::make_unique<Tests::MemoryDiskReader>(image),
             NTFS_BROWSER_NAMESPACE::VolumeOptions{.max_cache_bytes = ceiling});
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>
      root(volume);
  REQUIRE(root.ParseFileRecord(
      static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());
  const auto& data_attrs =
      root.GetAttr(NTFS_BROWSER_NAMESPACE::Attr::Type::Data);
  REQUIRE(data_attrs.size() == 1);

  std::vector<BYTE> buffer(
      gsl::narrow<size_t>(data_attrs.front()->GetDataSize()));
  for (int pass = 0; pass < release_passes; pass++) {
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<
        NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>
        again(volume);
    REQUIRE(again.ParseFileRecord(
        static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::Root)));
    REQUIRE(again.ParseAttrs());
    const auto& again_attrs =
        again.GetAttr(NTFS_BROWSER_NAMESPACE::Attr::Type::Data);
    REQUIRE(again_attrs.size() == 1);

    std::vector<BYTE> pass_bytes(buffer.size());
    const std::optional<ULONGLONG> read =
        again_attrs.front()->ReadData(0, pass_bytes);
    REQUIRE(read.has_value());
    pass_bytes.resize(gsl::narrow<size_t>(*read));
    if (pass == 0) {
      buffer = std::move(pass_bytes);
    } else {
      // Every later pass must read what the first one read.
      REQUIRE(pass_bytes == buffer);
    }
  }
  return buffer;
}

}  // namespace

TEST_CASE(
    "A compressed file reads the same bytes once its cache blocks were "
    "released",
    "[cache][compression]") {
  const auto builders = std::to_array<CompressedBuilder>(
      {&Tests::BuildFakeNtfsImageWithCompressedFile,
       &Tests::BuildFakeNtfsImageWithStoredCompressionUnit,
       &Tests::BuildFakeNtfsImageWithFragmentedCompressedFile,
       &Tests::BuildFakeNtfsImageWithTrailingPartialCompressionUnit});

  for (const CompressedBuilder builder : builders) {
    const std::vector<BYTE> image = builder();
    const std::vector<BYTE> expected = ReadWholeCompressed(
        image, NTFS_BROWSER_NAMESPACE::Cache::unlimited_bytes);

    REQUIRE_FALSE(expected.empty());
    CHECK(ReadWholeCompressed(image, release_block) == expected);
    CHECK(ReadWholeCompressed(image, 0) == expected);
  }
}

#endif  // NTFS_BROWSER_ENABLE_DECOMPRESSION

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))

namespace {

// First cluster of the encrypted file in these tests. The file runs from
// here across the 64 KiB boundary at LCN 128, clear of the $EFS stream (LCN
// 60).
constexpr DWORD spanning_first_lcn = 70;

// Clusters of the file: 61 KiB, so it crosses that boundary.
constexpr DWORD spanning_clusters = 61;

// Bytes of the file's plaintext: a little under its clusters, so the last
// sector is partial.
constexpr size_t spanning_size = (spanning_clusters * 1024ULL) - 100;

// Where the second run of the fragmented layout starts, past the end of the
// first one, and its length. The first run keeps the other clusters.
constexpr DWORD fragment_second_lcn = 140;
constexpr DWORD fragment_second_clusters = 20;

// Bytes of one FullCache block.
constexpr std::uint64_t efs_release_block = 65536;

// An encrypted file of one unnamed stream laid over runs, and the provider
// that decrypts it. The image is padded to whole blocks, so that FullCache
// caches the block past the file instead of falling back to a direct read.
Fixture MakeSpanningFixture(
    const std::vector<NTFS_BROWSER_NAMESPACE::Tests::FakeDataRun>& runs) {
  Fixture fixture;
  fixture.plaintext =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(spanning_size);
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256);

  const TestEfsEntry user = TestUser();
  fixture.provider = std::make_shared<TestKeyProvider>();
  fixture.provider->Add(
      user.thumbprint, user.wrapped_fek,
      NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes256, key));

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back(
      {.runs = runs,
       .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
           Algorithm::Aes256, key, fixture.plaintext),
       .real_size = spanning_size});
  fixture.image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(file);
  fixture.image.resize(4 * efs_release_block);
  return fixture;
}

// Reads the file at a ceiling: whole, then in small windows that cross the
// cluster runs, each on a volume with the same ceiling.
void CheckEncryptedFileReadsBack(const Fixture& fixture,
                                 std::uint64_t ceiling) {
  const NTFS_BROWSER_NAMESPACE::VolumeOptions options{.max_cache_bytes =
                                                          ceiling};
  auto opened = Open<Cache::Strategy::FullCache>(
      fixture.image, fixture.provider, std::nullopt, options);
  const auto& data = OnlyData(*opened.record);

  for (int pass = 0; pass < 3; pass++) {
    const std::optional<std::vector<BYTE>> whole =
        ReadAt<Cache::Strategy::FullCache>(data, 0, spanning_size);
    REQUIRE(whole.has_value());
    CHECK(*whole == fixture.plaintext);
  }

  // Windows that start and end inside different clusters and runs.
  constexpr size_t window = 5000;
  for (size_t offset = 0; offset < spanning_size; offset += 7919) {
    const size_t length = std::min(window, spanning_size - offset);
    const std::optional<std::vector<BYTE>> part =
        ReadAt<Cache::Strategy::FullCache>(data, offset, length);
    REQUIRE(part.has_value());
    CHECK(std::equal(part->begin(), part->end(),
                     fixture.plaintext.begin() +
                         static_cast<std::ptrdiff_t>(offset)));
  }
}

}  // namespace

TEST_CASE(
    "An encrypted file reads the same bytes once its cache blocks were "
    "released",
    "[cache][efs]") {
  const Fixture contiguous =
      MakeSpanningFixture({{spanning_first_lcn, spanning_clusters}});
  const Fixture fragmented = MakeSpanningFixture(
      {{spanning_first_lcn, spanning_clusters - fragment_second_clusters},
       {fragment_second_lcn, fragment_second_clusters}});

  for (const Fixture* fixture : {&contiguous, &fragmented}) {
    CheckEncryptedFileReadsBack(*fixture, Cache::unlimited_bytes);
    CheckEncryptedFileReadsBack(*fixture, efs_release_block);
    CheckEncryptedFileReadsBack(*fixture, 0);
  }
}

#endif
