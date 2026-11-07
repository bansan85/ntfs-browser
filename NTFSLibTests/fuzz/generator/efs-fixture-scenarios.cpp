#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/efs-fixture-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #define NTFS_FUZZGEN_HAS_EFS_SUPPORT 1
  #include <ntfs-browser/efs/efs.h>
  #include <ntfs-browser/test/efs/efs-test-support.h>
#endif

#include "data/header-non-resident.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;
using Probe = RecordedStream::Probe;

// The $Volume record, which the inputs about a damaged $EFS stream use.
constexpr ULONGLONG volume_record = 3;

// The type code of a $LOGGED_UTILITY_STREAM, the flag of an encrypted stream,
// and the names of the two utility streams.
constexpr DWORD type_logged_utility_stream = 0x100;
constexpr WORD attr_flag_encrypted = 0x4000;
constexpr std::wstring_view txf_data_name = L"$TXF_DATA";
constexpr std::wstring_view efs_name = L"$EFS";

// A non-resident attribute keeps its name after its 64 header bytes. Name and
// run list both end on a multiple of 8 bytes.
constexpr std::size_t non_resident_header_bytes = 0x40;
constexpr std::size_t alignment = 8;

// The run list of a stream with no cluster: its end byte.
constexpr BYTE end_of_runs = 0;

// The size the $EFS stream of the too-large input declares: 1 MiB, past the
// 64 KiB that the library reads.
constexpr ULONGLONG efs_size_too_large = 0x100000;

// Appends a non-resident attribute with no cluster to record idx: the name
// first, then the end byte of the run list. The attribute declares size bytes.
void PutEmptyStream(Image& image, ULONGLONG idx, DWORD type,
                    std::wstring_view name, WORD flags, ULONGLONG size) {
  std::vector<BYTE> body;
  for (const wchar_t unit : name) {
    body.push_back(static_cast<BYTE>(unit & 0xFF));
    body.push_back(static_cast<BYTE>(unit >> 8));
  }
  body.resize((body.size() + alignment - 1) / alignment * alignment, 0);
  const std::size_t runs_offset = non_resident_header_bytes + body.size();
  body.push_back(end_of_runs);
  body.resize((body.size() + alignment - 1) / alignment * alignment, 0);
  const std::size_t attr =
      RecordEdit::AppendNonResident(image, idx, type, body, 0, size, size);
  RecordEdit::Edit<Data::HeaderNonResident>(image, attr, [&](auto& header) {
    header.header.name_length = static_cast<BYTE>(name.size());
    header.header.name_offset = static_cast<WORD>(non_resident_header_bytes);
    header.header.flags = flags;
    header.data_run_offset = static_cast<WORD>(runs_offset);
  });
}

// The $Volume record of the extension volume holds an encrypted stream with no
// cluster, a utility stream beside the $EFS, and an $EFS of 1 MiB.
Image VolumeWithHugeEfs() {
  Image image = FakeImage::BuildFakeNtfsImageWithMftExtensionRecord();
  PutEmptyStream(image, volume_record, RecordEdit::type_data, {},
                 attr_flag_encrypted, 0);
  PutEmptyStream(image, volume_record, type_logged_utility_stream,
                 txf_data_name, 0, 0);
  PutEmptyStream(image, volume_record, type_logged_utility_stream, efs_name, 0,
                 efs_size_too_large);
  return image;
}

#ifdef NTFS_FUZZGEN_HAS_EFS_SUPPORT

namespace Efs = Test::Efs;
using NTFS_BROWSER_NAMESPACE::Efs::Algorithm;

// The file of the decrypt inputs holds 3000 bytes, not a multiple of a sector.
// The first builders counted the clusters of the stream in sectors of 512
// bytes, so the stream owns six clusters of the volume, and the ciphertext
// fills the first three.
constexpr std::size_t decrypt_plaintext_bytes = 3000;
constexpr DWORD decrypt_stream_lcn = 30;
constexpr DWORD decrypt_stream_clusters = 6;

// The user that the file is encrypted for. The first fixtures stored the FEK
// blob itself as the wrapped key.
constexpr BYTE decrypt_user_seed = 1;

// The file of the volume: one encrypted stream and its $EFS.
Image EncryptedWith(Algorithm algorithm) {
  const std::vector<BYTE> key = Efs::TestKey(algorithm);
  const Efs::TestEfsEntry user{Efs::TestThumbprint(decrypt_user_seed),
                               Efs::MakeFekBlob(algorithm, key)};
  FakeImage::FakeEncryptedFile file;
  file.efs_stream = Efs::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back(FakeImage::FakeEncryptedStream{
      .runs = {FakeImage::FakeDataRun{.lcn = decrypt_stream_lcn,
                                      .clusters = decrypt_stream_clusters}},
      .cluster_bytes = Efs::EfsEncrypt(
          algorithm, key, Efs::PlaintextPattern(decrypt_plaintext_bytes)),
      .real_size = decrypt_plaintext_bytes});
  return FakeImage::BuildFakeNtfsImageWithEncryptedFile(file);
}

#else

// A build without an EFS backend skips these inputs before it calls make.
enum class Algorithm : int { Aes128, Aes192, Aes256, _3Des, Desx };

Image EncryptedWith(Algorithm /*algorithm*/) {
  throw std::logic_error("this build has no EFS backend");
}

#endif

}  // namespace

void EfsFixtureScenarios::Register(ScenarioRegistry& registry) {
  struct Decrypt {
    const char* name;
    Algorithm algorithm;
    Probe probe;
    LegacyFixtures::Boot boot;
  };

  const Decrypt decrypts[] = {
      {"efs_decrypt_aes128_no_cache", Algorithm::Aes128, Probe::Main,
       LegacyFixtures::Boot::UnsetTotalSectors},
      {"efs_decrypt_aes192_no_cache", Algorithm::Aes192, Probe::Main,
       LegacyFixtures::Boot::UnsetTotalSectors},
      {"efs_decrypt_aes256_no_cache", Algorithm::Aes256, Probe::Main,
       LegacyFixtures::Boot::UnsetTotalSectors},
      {"efs_decrypt_3des_no_cache", Algorithm::_3Des, Probe::Main,
       LegacyFixtures::Boot::UnsetTotalSectors},
      {"efs_decrypt_desx_full_cache", Algorithm::Desx, Probe::FullCache,
       LegacyFixtures::Boot::PatchedTotalSectors},
  };
  LegacyFixtures::Register(registry,
                           {{.name = "filerecord_efs_stream_too_large",
                             .make = VolumeWithHugeEfs,
                             .needs = Feature::Efs}});
  for (const Decrypt& decrypt : decrypts) {
    LegacyFixtures::Register(registry, {{.name = decrypt.name,
                                         .make =
                                             [algorithm = decrypt.algorithm] {
                                               return EncryptedWith(algorithm);
                                             },
                                         .boot = decrypt.boot,
                                         .probe = decrypt.probe,
                                         .needs = Feature::Efs}});
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
