#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/efs/efs-test-support.h>
#include <ntfs-browser/test/efs/efs-tests-support.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/scripted-disk-reader.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "cache/strategy.h"
#include "disk-reader.h"
#include "efs/fek.h"
#include "io/file-record.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "volume-options.h"
#include "win-types.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestEfs = NTFS_BROWSER_NAMESPACE::Test::Efs;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Test::Efs::Algorithm;
using NTFS_BROWSER_NAMESPACE::Test::Efs::TestEfsEntry;
using NTFS_BROWSER_NAMESPACE::Test::Efs::TestKeyProvider;

namespace {

// One byte past the largest $EFS stream the library reads (64 KiB).
constexpr size_t oversized_efs_stream = (size_t{64} * 1024) + 1;

// Size of the file the fixtures encrypt: less than one cluster.
constexpr size_t file_size = 1000;

// First byte of the $EFS stream of the fake image, at LCN 60 of 1 KiB.
constexpr ULONGLONG efs_stream_offset =
    ULONGLONG{TestFakeImage::fake_efs_stream_lcn} *
    TestFakeImage::fake_cluster_size;

// An encrypted file of one stream, the key provider that opens it, and the
// plaintext it reads back as. what shapes the file before the image is built.
struct Encrypted {
  TestFakeImage::FakeEncryptedFile file;
  std::shared_ptr<TestKeyProvider> provider;
  std::vector<BYTE> plaintext;
};

Encrypted MakeEncrypted() {
  Encrypted encrypted;
  encrypted.plaintext = TestEfs::PlaintextPattern(file_size);
  const std::vector<BYTE> key = TestEfs::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  encrypted.provider = std::make_shared<TestKeyProvider>();
  encrypted.provider->Add(user.thumbprint, user.wrapped_fek,
                          TestEfs::MakeFekBlob(Algorithm::Aes256, key));
  encrypted.file.efs_stream = TestEfs::MakeEfsStream(std::span(&user, 1));
  encrypted.file.streams.push_back(
      {.runs = {{first_stream_lcn, 1}},
       .cluster_bytes =
           TestEfs::EfsEncrypt(Algorithm::Aes256, key, encrypted.plaintext),
       .real_size = file_size});
  return encrypted;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "Another utility stream beside $EFS does not hide the keys",
    "[efs][efs-limits]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  Encrypted encrypted = MakeEncrypted();
  encrypted.file.other_logged_stream_name = L"$TXF_DATA";

  const Opened<S> opened = OpenVolume<S>(
      TestFakeImage::BuildFakeNtfsImageWithEncryptedFile(encrypted.file),
      encrypted.provider);

  CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, file_size) ==
        encrypted.plaintext);
}

TEMPLATE_TEST_CASE_SIG("An $EFS stream above 64 KiB is not read",
                       "[efs][efs-limits]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  Encrypted encrypted = MakeEncrypted();
  encrypted.file.efs_stream.assign(oversized_efs_stream, 0);

  (void)TestSupport::TakeCapturedLog();
  const Opened<S> opened = OpenVolume<S>(
      TestFakeImage::BuildFakeNtfsImageWithEncryptedFile(encrypted.file),
      encrypted.provider);

  CHECK_THAT(TestSupport::TakeCapturedLog(),
             ContainsSubstring("$EFS stream is too large"));
  CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, file_size).has_value());
}

TEST_CASE("An $EFS stream that cannot be read leaves the data locked",
          "[efs][efs-limits]") {
  constexpr Cache::Strategy S = Cache::Strategy::NoCache;
  Encrypted encrypted = MakeEncrypted();
  auto reader = std::make_unique<TestSupport::ScriptedDiskReader>(
      TestFakeImage::BuildFakeNtfsImageWithEncryptedFile(encrypted.file));
  // The $EFS stream is one cluster; the data stream sits elsewhere.
  reader->FaultRange(efs_stream_offset,
                     efs_stream_offset + TestFakeImage::fake_cluster_size,
                     TestSupport::ScriptedDiskReader::Fault::Fail);
  NtfsVolume<S> volume(std::move(reader), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(encrypted.provider);

  (void)TestSupport::TakeCapturedLog();
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  static_cast<void>(record.ParseAttrs());

  CHECK_THAT(TestSupport::TakeCapturedLog(),
             ContainsSubstring("Cannot read the $EFS stream"));
  CHECK_FALSE(ReadAt<S>(OnlyData<S>(record), 0, file_size).has_value());
}
