// MakePfxKeyProvider() - this whole file's subject - only exists on Windows
// (PFX import goes through CryptoAPI/CNG): see include/ntfs-browser/efs/efs.h.
#ifdef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <array>
  #include <filesystem>
  #include <fstream>
  #include <memory>
  #include <optional>
  #include <random>
  #include <string>
  #include <string_view>
  #include <vector>

  #include <catch2/catch_test_macros.hpp>
  #include <gsl/narrow>

  #include <ntfs-browser/attr/base.h>
  #include <ntfs-browser/cache/strategy.h>
  #include <ntfs-browser/efs/efs.h>
  #include <ntfs-browser/io/file-record.h>
  #include <ntfs-browser/ntfs-volume.h>
  #include <ntfs-browser/volume-options.h>

  #include "md5-test-support.h"
  #include "nps-ntfs1-test-support.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Attr::AttrBase;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Efs::MakePfxKeyProvider;

namespace {

// The only Encrypted/* file whose $EFS entry matches a provided key.
constexpr std::string_view reencrypted_file = "logfile1.txt";

// One of the two EFS recovery keys narrative.txt promises at the corpus
// root, both unlocking the same certificate: exported without a password,
// and exported with the password "password".
struct EfsKey {
  std::string_view pfx_name;
  std::wstring_view password;
};

constexpr std::array<EfsKey, 2> efs_keys{{
    {"EFS-key-no-password.pfx", L""},
    {"EFS-key-password.pfx", L"password"},
}};

// Writes data to a fresh temp file and returns its path. MakePfxKeyProvider
// only reads a PFX from the host filesystem, not from inside the volume, so
// the key extracted from the image has to land on disk first.
std::filesystem::path WriteTempFile(std::string_view label,
                                    const std::vector<BYTE>& data) {
  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfs1-" + std::wstring(label.begin(), label.end()) + L"-" +
       std::to_wstring(rd()) + L".pfx");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out.write(reinterpret_cast<const char*>(data.data()),
            gsl::narrow<std::streamsize>(data.size()));
  REQUIRE(out.good());
  return path;
}

}  // namespace

TEST_CASE(
    "Encrypted files decrypt or fail to decrypt as their own $EFS entry "
    "dictates (NPS ntfs1, gen2)",
    "[nps][integration][efs]") {
  NtfsBrowserTests::RequireCorpusImage(NtfsBrowserTests::ntfs1_image);

  NtfsVolume<Cache::Strategy::NoCache> volume(
      NtfsBrowserTests::OpenNtfs1Image(), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  NtfsBrowserTests::OpenRootDir(root);

  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> encrypted_dir(volume);
  NtfsBrowserTests::OpenRootDir(encrypted_dir);
  NtfsBrowserTests::OpenSubDir(encrypted_dir, "Encrypted");

  for (const EfsKey& key : efs_keys) {
    INFO("key " << key.pfx_name);

    // The key file itself sits unencrypted at the volume root, so a plain
    // read recovers it.
    const std::vector<BYTE> pfx_bytes =
        NtfsBrowserTests::ReadFile(volume, root, key.pfx_name);
    const std::filesystem::path pfx_path =
        WriteTempFile(key.pfx_name, pfx_bytes);
    const std::shared_ptr<NtfsBrowser::Efs::IEfsKeyProvider> provider =
        MakePfxKeyProvider(pfx_path, key.password);
    std::filesystem::remove(pfx_path);
    REQUIRE(provider != nullptr);
    volume.SetEfsKeyProvider(provider);

    for (const NtfsBrowserTests::KnownFile& file :
         NtfsBrowserTests::known_files) {
      INFO("file " << file.name);

      NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> stream_owner(
          volume);
      NtfsBrowserTests::OpenFile(stream_owner, encrypted_dir, file.name);
      const NtfsBrowser::Attr::AttrBase<Cache::Strategy::NoCache>* stream =
          stream_owner.FindStream({});
      REQUIRE(stream != nullptr);
      CHECK(stream->GetDataSize() == file.size);

      std::vector<BYTE> data(stream->GetDataSize());
      const std::optional<ULONGLONG> read = stream->ReadData(0, data);

      // Only this file was rewritten after the volume's EFS certificate
      // changed.
      if (file.name == reencrypted_file) {
        REQUIRE(read == data.size());
  #ifdef NTFS_TEST_HAS_MD5
        CHECK(NtfsBrowserTests::Md5Hex(data) == file.md5);
  #endif
      } else {
        CHECK(read == std::nullopt);
      }
    }
  }
}

#endif  // _WIN32
