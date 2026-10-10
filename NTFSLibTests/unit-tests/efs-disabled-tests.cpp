// This file's whole purpose is the configuration where neither EFS backend
// is compiled: efs-tests.cpp (and the Crypto++-based fixtures it needs) are
// excluded there, so this is the only place that scenario gets covered.
#if !(defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
      (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)))

  #include <catch2/catch_template_test_macros.hpp>
  #include <gsl/narrow>

  #include <ntfs-browser/attr/type.h>
  #include <ntfs-browser/cache/strategy.h>
  #include <ntfs-browser/io/file-record.h>
  #include <ntfs-browser/mft/idx.h>
  #include <ntfs-browser/ntfs-volume.h>

  #include "fake-ntfs-image.h"
  #include "memory-disk-reader.h"

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads back as raw ciphertext when no EFS backend "
    "is compiled in",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> onDisk(NtfsBrowserTests::fake_cluster_size, 0x42);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.streams.push_back({.runs = {{30, 1}},
                          .cluster_bytes = onDisk,
                          .real_size = onDisk.size(),
                          .flagged_encrypted = true});

  NtfsVolume<S> volume(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)));
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(nullptr);

  NtfsBrowser::Io::FileRecord<S> record(volume);
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
