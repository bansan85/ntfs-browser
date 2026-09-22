#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/efs.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "efs-test-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace fs = std::filesystem;

using NtfsBrowser::Efs::MakePfxKeyProvider;
using NtfsBrowserTests::Algorithm;

namespace
{

// One throwaway certificate of NTFSLibTests/unit-tests/data, made in memory
// for these tests alone. Its key is of no value. "wrapped" holds a FEK
// wrapped with its public key, in the little-endian byte order of a real
// $EFS stream.
struct TestPfx
{
  const char* name;
  const char* thumbprint;
};

// One key in CNG storage, one in CryptoAPI storage, which is what a real EFS
// certificate uses. Both are unwrapped with different calls.
constexpr TestPfx kCng{"efs-test-cng",
                       "02E4E09AA6DFDC115B5EDF1435D7C39D04733175"};
constexpr TestPfx kCapi{"efs-test-capi",
                        "74463DBBC1B1333314670FB8665E8398B203207E"};

// The password every test PFX is protected with.
constexpr std::wstring_view kPassword = L"efs-test";

fs::path DataFile(const std::string& name)
{
  return fs::path(NTFS_EFS_TEST_DATA_DIR) / name;
}

std::vector<BYTE> ReadWholeFile(const fs::path& path)
{
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.good());
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

std::vector<BYTE> Thumbprint(const TestPfx& pfx)
{
  const std::string hex = pfx.thumbprint;
  std::vector<BYTE> bytes;
  for (size_t i = 0; i < hex.size(); i += 2)
  {
    bytes.push_back(
        static_cast<BYTE>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return bytes;
}

std::vector<BYTE> WrappedFek(const TestPfx& pfx)
{
  return ReadWholeFile(DataFile(std::string(pfx.name) + ".wrapped-fek.bin"));
}

// The FEK blob the fixtures wrapped: an AES-256 header, then 32 bytes.
std::vector<BYTE> ExpectedFek()
{
  std::vector<BYTE> key(32);
  for (size_t i = 0; i < key.size(); ++i)
  {
    key[i] = static_cast<BYTE>((i + 1) * 3);
  }
  return NtfsBrowserTests::MakeFekBlob(Algorithm::kAes256, key);
}

}  // namespace

TEST_CASE("A PFX key provider unwraps a FEK, whichever way the key is stored",
          "[efs][pfx]")
{
  for (const TestPfx& pfx : {kCng, kCapi})
  {
    INFO("certificate " << pfx.name);
    const std::shared_ptr<NtfsBrowser::Efs::IEfsKeyProvider> provider =
        MakePfxKeyProvider(DataFile(std::string(pfx.name) + ".pfx"), kPassword);
    REQUIRE(provider != nullptr);

    const std::vector<BYTE> wrapped = WrappedFek(pfx);
    CHECK(provider->UnwrapFek(Thumbprint(pfx), wrapped) == ExpectedFek());

    std::vector<BYTE> tampered = wrapped;
    tampered[100] ^= 0x40;
    CHECK_FALSE(provider->UnwrapFek(Thumbprint(pfx), tampered).has_value());
  }
}

TEST_CASE("A PFX key provider only knows the certificates of its own file",
          "[efs][pfx]")
{
  const auto provider =
      MakePfxKeyProvider(DataFile("efs-test-cng.pfx"), kPassword);
  REQUIRE(provider != nullptr);

  CHECK_FALSE(
      provider->UnwrapFek(Thumbprint(kCapi), WrappedFek(kCapi)).has_value());
}

TEST_CASE("A PFX that cannot be opened gives no provider", "[efs][pfx]")
{
  (void)NtfsBrowserTests::TakeCapturedLog();

  CHECK(MakePfxKeyProvider(DataFile("efs-test-cng.pfx"), L"wrong") == nullptr);
  CHECK_THAT(NtfsBrowserTests::TakeCapturedLog(),
             Catch::Matchers::ContainsSubstring("Cannot import the PFX"));

  CHECK(MakePfxKeyProvider(DataFile("garbage.pfx"), kPassword) == nullptr);
  CHECK(MakePfxKeyProvider(DataFile("no-such-file.pfx"), kPassword) == nullptr);
  CHECK_THAT(NtfsBrowserTests::TakeCapturedLog(),
             Catch::Matchers::ContainsSubstring("Cannot read a PFX"));
}

TEST_CASE("A stream decrypts end to end with a PFX key provider", "[efs][pfx]")
{
  using NtfsBrowser::Strategy;

  for (const TestPfx& pfx : {kCng, kCapi})
  {
    INFO("certificate " << pfx.name);
    const std::vector<BYTE> blob = ExpectedFek();
    const std::vector<BYTE> key(blob.begin() + 16, blob.end());
    const std::vector<BYTE> plaintext =
        NtfsBrowserTests::PlaintextPattern(1500);

    NtfsBrowserTests::TestEfsEntry user;
    const std::vector<BYTE> thumbprint = Thumbprint(pfx);
    std::copy(thumbprint.begin(), thumbprint.end(), user.thumbprint.begin());
    user.wrapped_fek = WrappedFek(pfx);

    NtfsBrowserTests::FakeEncryptedFile file;
    file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
    file.streams.push_back({.runs = {{30, 2}},
                            .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                                Algorithm::kAes256, key, plaintext),
                            .real_size = plaintext.size()});

    NtfsBrowser::NtfsVolume<Strategy::NO_CACHE> volume(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
            NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)));
    REQUIRE(volume.IsVolumeOK());
    volume.SetEfsKeyProvider(MakePfxKeyProvider(
        DataFile(std::string(pfx.name) + ".pfx"), kPassword));

    NtfsBrowser::FileRecord<Strategy::NO_CACHE> record(volume);
    REQUIRE(record.ParseFileRecord(
        static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::ROOT)));
    REQUIRE(record.ParseAttrs());

    const auto& data = record.getAttr(NtfsBrowser::AttrType::DATA);
    REQUIRE(data.size() == 1);
    std::vector<BYTE> buffer(plaintext.size());
    const auto read = data.front()->ReadData(0, buffer);
    REQUIRE(read.has_value());
    CHECK(buffer == plaintext);
  }
}
