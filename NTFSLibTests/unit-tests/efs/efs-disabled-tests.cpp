// This file's whole purpose is the configuration where neither EFS backend
// is compiled: the other EFS tests (and the Crypto++-based fixtures they need)
// are excluded there, so this is the only place that scenario gets covered.
#if !(defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
      (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)))

  #include <catch2/catch_template_test_macros.hpp>
  #include <gsl/narrow>

  #include <ntfs-browser/attr/type.h>
  #include <ntfs-browser/cache/strategy.h>
  #include <ntfs-browser/io/file-record.h>
  #include <ntfs-browser/mft/idx.h>
  #include <ntfs-browser/namespace.h>
  #include <ntfs-browser/ntfs-volume.h>

  #include "fake-ntfs-image.h"
  #include "memory-disk-reader.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads back as raw ciphertext when no EFS backend "
    "is compiled in",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> onDisk(
      NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size, 0x42);

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.streams.push_back({.runs = {{30, 1}},
                          .cluster_bytes = onDisk,
                          .real_size = onDisk.size(),
                          .flagged_encrypted = true});

  NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(
              file)));
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(nullptr);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(record.ParseAttrs());

  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);

  std::vector<BYTE> buffer(onDisk.size(), 0xCC);
  const std::optional<ULONGLONG> read = data.front()->ReadData(0, buffer);
  REQUIRE(read.has_value());
  buffer.resize(gsl::narrow<size_t>(*read));
  CHECK(buffer == onDisk);
}

#endif
