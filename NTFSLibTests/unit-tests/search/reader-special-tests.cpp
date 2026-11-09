#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "efs/efs.h"
#include "reader-support.h"
#include "scan-support.h"
#include "search/reader-probe.h"
#include "win-types.h"

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #include <ntfs-browser/test/efs/efs-test-support.h>
  #define NTFS_BROWSER_READER_TESTS_EFS
#endif

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using TestSearch::TempImage;
using Bytes = std::vector<std::uint8_t>;

namespace {

// The handle of the root record, which these fixtures replace.
constexpr Lib::Handle root_handle{.record = 5};

// Makes the next Readers decrypt with provider, and puts the seam back when it
// goes out of scope: no test reaches the certificate store of the user.
class KeyProviderScope {
 public:
  explicit KeyProviderScope(
      std::shared_ptr<NTFS_BROWSER_NAMESPACE::Efs::IEfsKeyProvider> provider) {
    Lib::SetReaderKeyProviderForTests(std::move(provider));
  }

  KeyProviderScope(const KeyProviderScope&) = delete;
  KeyProviderScope& operator=(const KeyProviderScope&) = delete;
  KeyProviderScope(KeyProviderScope&&) = delete;
  KeyProviderScope& operator=(KeyProviderScope&&) = delete;

  ~KeyProviderScope() { Lib::ClearReaderKeyProviderForTests(); }
};

}  // namespace

#ifdef NTFS_BROWSER_READER_TESTS_EFS

namespace {

namespace TestEfs = NTFS_BROWSER_NAMESPACE::Test::Efs;
using TestEfs::Algorithm;

// Where the encrypted stream sits, in clusters.
constexpr std::uint32_t stream_lcn = 30;

// Size of the plaintext of the encrypted file: several sectors, not a multiple
// of one.
constexpr std::size_t plaintext_size = 3000;

// An encrypted file, the plaintext it holds, and a provider that owns its key.
struct EncryptedVolume {
  std::vector<BYTE> image;
  std::vector<BYTE> plaintext;
  std::shared_ptr<TestEfs::TestKeyProvider> provider;
};

EncryptedVolume MakeEncryptedVolume() {
  EncryptedVolume volume;
  volume.plaintext = TestEfs::PlaintextPattern(plaintext_size);
  const std::vector<BYTE> key = TestEfs::TestKey(Algorithm::Aes256);
  const TestEfs::TestEfsEntry user{TestEfs::TestThumbprint(1),
                                   std::vector<BYTE>(32, 0xAB)};
  volume.provider = std::make_shared<TestEfs::TestKeyProvider>();
  volume.provider->Add(user.thumbprint, user.wrapped_fek,
                       TestEfs::MakeFekBlob(Algorithm::Aes256, key));

  TestFake::FakeEncryptedFile file;
  file.efs_stream = TestEfs::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{stream_lcn, 3}},
                          .cluster_bytes = TestEfs::EfsEncrypt(
                              Algorithm::Aes256, key, volume.plaintext),
                          .real_size = plaintext_size});
  volume.image = TestFake::BuildFakeNtfsImageWithEncryptedFile(file);
  return volume;
}

}  // namespace

TEST_CASE("A Reader decrypts an EFS stream with the key it is given",
          "[search][reader][efs]") {
  const EncryptedVolume volume = MakeEncryptedVolume();
  const TempImage image(volume.image);
  const KeyProviderScope scope(volume.provider);
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());

  const auto streams = reader.GetStreams(root_handle);
  REQUIRE(streams);
  REQUIRE(streams.value.size() == 1);
  CHECK(streams.value[0].encrypted);

  SECTION("the whole stream") {
    Bytes bytes(plaintext_size, 0xEE);

    const auto read = reader.Read(root_handle, u"", 0, bytes);

    REQUIRE(read);
    CHECK(read.value == plaintext_size);
    CHECK(bytes == Bytes(volume.plaintext.begin(), volume.plaintext.end()));
  }

  SECTION("a slice that is not aligned") {
    Bytes bytes(700, 0xEE);

    const auto read = reader.Read(root_handle, u"", 511, bytes);

    REQUIRE(read);
    CHECK(read.value == 700);
    CHECK(bytes == Bytes(volume.plaintext.begin() + 511,
                         volume.plaintext.begin() + 1211));
  }

  SECTION("a copy to a sink") {
    Bytes copied;

    const auto result = reader.CopyTo(
        root_handle, u"", [&copied](std::span<const std::uint8_t> chunk) {
          copied.insert(copied.end(), chunk.begin(), chunk.end());
          return true;
        });

    REQUIRE(result);
    CHECK(copied == Bytes(volume.plaintext.begin(), volume.plaintext.end()));
  }
}

TEST_CASE("A Reader fails with Encrypted when no key opens the stream",
          "[search][reader][efs]") {
  const EncryptedVolume volume = MakeEncryptedVolume();
  const TempImage image(volume.image);
  const KeyProviderScope scope(nullptr);
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());
  (void)TestSupport::TakeCapturedLog();
  Bytes bytes(plaintext_size, 0xEE);

  const auto read = reader.Read(root_handle, u"", 0, bytes);

  CHECK(read.error == Lib::ReadError::Encrypted);
  CHECK(bytes == Bytes(plaintext_size, 0));
}

TEST_CASE("A Reader fails with Encrypted when the key is not the file's",
          "[search][reader][efs]") {
  const EncryptedVolume volume = MakeEncryptedVolume();
  const TempImage image(volume.image);
  const KeyProviderScope scope(std::make_shared<TestEfs::TestKeyProvider>());
  Lib::Reader reader({.target = image.Target()});
  (void)TestSupport::TakeCapturedLog();
  Bytes bytes(plaintext_size, 0xEE);

  const auto read = reader.Read(root_handle, u"", 0, bytes);

  CHECK(read.error == Lib::ReadError::Encrypted);
}

#endif  // NTFS_BROWSER_READER_TESTS_EFS

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION

TEST_CASE("A Reader reads a compressed stream decompressed",
          "[search][reader]") {
  const TempImage image(TestFake::BuildFakeNtfsImageWithCompressedFile());
  const KeyProviderScope scope(nullptr);
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());

  const auto streams = reader.GetStreams(root_handle);
  REQUIRE(streams);
  REQUIRE(streams.value.size() == 1);
  CHECK(streams.value[0].size == TestFake::xca_lznt1_example_decompressed_size);

  Bytes bytes(TestFake::xca_lznt1_example_decompressed_size, 0xEE);
  const auto read = reader.Read(root_handle, u"", 0, bytes);

  REQUIRE(read);
  CHECK(read.value == bytes.size());
  CHECK(std::equal(TestFake::xca_lznt1_example_decompressed.begin(),
                   TestFake::xca_lznt1_example_decompressed.end(),
                   bytes.begin()));
}

#else

TEST_CASE("A Reader fails cleanly on a compressed stream without LZNT1",
          "[search][reader]") {
  const TempImage image(TestFake::BuildFakeNtfsImageWithCompressedFile());
  const KeyProviderScope scope(nullptr);
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());
  (void)TestSupport::TakeCapturedLog();
  Bytes bytes(64, 0xEE);

  const auto read = reader.Read(root_handle, u"", 0, bytes);

  CHECK_FALSE(read);
  CHECK(bytes == Bytes(64, 0xEE));
}

#endif
