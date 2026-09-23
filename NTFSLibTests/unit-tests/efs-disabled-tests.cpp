#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

// This file's whole purpose is the configuration where neither EFS backend
// is compiled: efs-tests.cpp (and the Crypto++-based fixtures it needs) are
// excluded there, so this is the only place that scenario gets covered.
#if !(defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
      (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)))

  #include <ntfs-browser/data/attr-type.h>
  #include <ntfs-browser/file-record.h>
  #include <ntfs-browser/mft-idx.h>
  #include <ntfs-browser/ntfs-volume.h>
  #include <ntfs-browser/strategy.h>

  #include "fake-ntfs-image.h"
  #include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads back as raw ciphertext when no EFS backend "
    "is compiled in",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  const std::vector<BYTE> onDisk(NtfsBrowserTests::kFakeClusterSize, 0x42);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.streams.push_back({.runs = {{30, 1}},
                          .cluster_bytes = onDisk,
                          .real_size = onDisk.size(),
                          .flagged_encrypted = true});

  NtfsVolume<S> volume(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)));
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(nullptr);

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  const auto& data = record.getAttr(AttrType::DATA);
  REQUIRE(data.size() == 1);

  std::vector<BYTE> buffer(onDisk.size(), 0xCC);
  const std::optional<ULONGLONG> read = data.front()->ReadData(0, buffer);
  REQUIRE(read.has_value());
  buffer.resize(static_cast<size_t>(*read));
  CHECK(buffer == onDisk);
}

#endif
