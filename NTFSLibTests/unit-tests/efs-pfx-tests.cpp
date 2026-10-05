// MakePfxKeyProvider() - this whole file's subject - only exists on Windows
// (PFX import goes through CryptoAPI/CNG): see include/ntfs-browser/efs.h.
#ifdef _WIN32

  #include <algorithm>
  #include <filesystem>
  #include <fstream>
  #include <iterator>
  #include <memory>
  #include <string>
  #include <vector>

  #include <catch2/catch_template_test_macros.hpp>
  #include <catch2/catch_test_macros.hpp>
  #include <catch2/matchers/catch_matchers_string.hpp>
  #include <gsl/narrow>

  #include <ntfs-browser/efs.h>
  #include <ntfs-browser/file-record.h>
  #include <ntfs-browser/mft-idx.h>
  #include <ntfs-browser/ntfs-volume.h>

  #include "efs-test-support.h"
  #include "fake-ntfs-image.h"
  #include "memory-disk-reader.h"
  #include "test-log-sink.h"

namespace Fs = std::filesystem;

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
constexpr TestPfx cng_value{"efs-test-cng",
                            "02E4E09AA6DFDC115B5EDF1435D7C39D04733175"};
constexpr TestPfx capi_value{"efs-test-capi",
                             "74463DBBC1B1333314670FB8665E8398B203207E"};

// The password every test PFX is protected with.
constexpr std::wstring_view password_value = L"efs-test";

// Radix of the hex digits in a thumbprint string.
constexpr int hex_radix = 16;

// Size of the AES-256 key the fixtures wrapped.
constexpr size_t fek_key_size = 32;

Fs::path DataFile(const std::string& name)
{
  return Fs::path(NTFS_EFS_TEST_DATA_DIR) / name;
}

std::vector<BYTE> ReadWholeFile(const Fs::path& path)
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
        gsl::narrow<BYTE>(std::stoi(hex.substr(i, 2), nullptr, hex_radix)));
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
  std::vector<BYTE> key(fek_key_size);
  for (size_t i = 0; i < key.size(); ++i)
  {
    // i < key.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    key[i] = gsl::narrow<BYTE>((i + 1) * 3);
  }
  return NtfsBrowserTests::MakeFekBlob(Algorithm::Aes256, key);
}

}  // namespace

TEST_CASE("A PFX key provider unwraps a FEK, whichever way the key is stored",
          "[efs][pfx]")
{
  for (const TestPfx& pfx : {cng_value, capi_value})
  {
    INFO("certificate " << pfx.name);
    const std::shared_ptr<NtfsBrowser::Efs::IEfsKeyProvider> provider =
        MakePfxKeyProvider(DataFile(std::string(pfx.name) + ".pfx"),
                           password_value);
    REQUIRE(provider != nullptr);

    const std::vector<BYTE> wrapped = WrappedFek(pfx);
    CHECK(provider->UnwrapFek(Thumbprint(pfx), wrapped) == ExpectedFek());

    std::vector<BYTE> tampered = wrapped;
    tampered.at(100) ^= 0x40;
    CHECK_FALSE(provider->UnwrapFek(Thumbprint(pfx), tampered).has_value());
  }
}

TEST_CASE("A PFX key provider only knows the certificates of its own file",
          "[efs][pfx]")
{
  const auto provider =
      MakePfxKeyProvider(DataFile("efs-test-cng.pfx"), password_value);
  REQUIRE(provider != nullptr);

  CHECK_FALSE(
      provider->UnwrapFek(Thumbprint(capi_value), WrappedFek(capi_value))
          .has_value());
}

TEST_CASE("A PFX that cannot be opened gives no provider", "[efs][pfx]")
{
  (void)NtfsBrowserTests::TakeCapturedLog();

  CHECK(MakePfxKeyProvider(DataFile("efs-test-cng.pfx"), L"wrong") == nullptr);
  CHECK_THAT(NtfsBrowserTests::TakeCapturedLog(),
             Catch::Matchers::ContainsSubstring("Cannot import the PFX"));

  CHECK(MakePfxKeyProvider(DataFile("garbage.pfx"), password_value) == nullptr);
  CHECK(MakePfxKeyProvider(DataFile("no-such-file.pfx"), password_value) ==
        nullptr);
  CHECK_THAT(NtfsBrowserTests::TakeCapturedLog(),
             Catch::Matchers::ContainsSubstring("Cannot read a PFX"));
}

TEST_CASE("An oversized PFX is refused by its size, before any read",
          "[efs][pfx]")
{
  constexpr std::uintmax_t just_over_the_limit = 16ULL * 1024 * 1024 + 1;
  const Fs::path path =
      Fs::temp_directory_path() / "ntfs-browser-oversized.pfx";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    const std::vector<char> chunk(1024 * 1024, 'x');
    for (std::uintmax_t written = 0;
         written + chunk.size() <= just_over_the_limit; written += chunk.size())
    {
      out.write(chunk.data(), gsl::narrow<std::streamsize>(chunk.size()));
    }
    out.put('x');
  }
  REQUIRE(Fs::file_size(path) == just_over_the_limit);

  (void)NtfsBrowserTests::TakeCapturedLog();
  const auto provider = MakePfxKeyProvider(path, password_value);
  const std::string log = NtfsBrowserTests::TakeCapturedLog();
  std::error_code ignored;
  Fs::remove(path, ignored);

  CHECK(provider == nullptr);
  CHECK_THAT(log, Catch::Matchers::ContainsSubstring("too large"));
}

TEMPLATE_TEST_CASE_SIG("A stream decrypts end to end with a PFX key provider",
                       "[efs][pfx]", ((NtfsBrowser::Strategy S), S),
                       NtfsBrowser::Strategy::NoCache,
                       NtfsBrowser::Strategy::FullCache)
{
  for (const TestPfx& pfx : {cng_value, capi_value})
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
                                Algorithm::Aes256, key, plaintext),
                            .real_size = plaintext.size()});

    NtfsBrowser::NtfsVolume<S> volume(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
            NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)));
    REQUIRE(volume.IsVolumeOK());
    volume.SetEfsKeyProvider(MakePfxKeyProvider(
        DataFile(std::string(pfx.name) + ".pfx"), password_value));

    NtfsBrowser::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(
        static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::Root)));
    REQUIRE(record.ParseAttrs());

    const auto& data = record.GetAttr(NtfsBrowser::AttrType::Data);
    REQUIRE(data.size() == 1);
    std::vector<BYTE> buffer(plaintext.size());
    const auto read = data.front()->ReadData(0, buffer);
    REQUIRE(read.has_value());
    CHECK(buffer == plaintext);
  }
}

#endif  // _WIN32
