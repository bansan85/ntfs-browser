#include "efs-fixture-scenarios.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "bpb-edit.h"
#include "hand-made-efs.h"
#include "legacy-fixtures.h"
#include "legacy-record.h"
#include "record-edit.h"
#include "win-types.h"

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #define NTFS_FUZZGEN_HAS_EFS_SUPPORT 1
  #include <ntfs-browser/test/efs/efs-test-support.h>

  #include "efs/efs.h"
#endif

#include "data/header-non-resident.h"
#include "data/ntfs-bpb.h"

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

// An encrypted file of a hand-made volume: a record in the layout of a real
// NTFS volume, with one encrypted stream and the $EFS value given.
struct HandMadeFile {
  ULONGLONG record;
  ULONGLONG clusters;
  ULONGLONG allocated_bytes;
  std::vector<BYTE> runs;
  std::vector<BYTE> efs;
};

void PutEncryptedFile(Image& image, const HandMadeFile& file) {
  constexpr WORD stream_id = 2;
  constexpr WORD efs_id = 1;
  LegacyRecord::Put(
      image, file.record,
      {.used_tail = 4,
       .next_attribute_id = 3,
       .record_number = static_cast<DWORD>(file.record),
       .attributes = {{.type = LegacyRecord::type_data,
                       .flags = LegacyRecord::flag_encrypted,
                       .id = stream_id,
                       .non_resident = true,
                       .last_vcn = file.clusters - 1,
                       .allocated_size = file.allocated_bytes,
                       .real_size = decrypt_plaintext_bytes,
                       .initialized_size = decrypt_plaintext_bytes,
                       .raw_runs = file.runs},
                      {.type = LegacyRecord::type_logged_utility_stream,
                       .name = L"$EFS",
                       .id = efs_id,
                       .value = file.efs}}});
}

// The keys of the hand-made entries: 16 bytes for AES with 128 bits, 32 for
// AES with 256 bits and for the entries that give a key of the wrong length.
constexpr std::size_t aes128_key_bytes = 16;
constexpr std::size_t aes256_key_bytes = 32;

// The FEK of AES-128 with a key of 16 bytes.
std::vector<BYTE> Aes128Fek() {
  return HandMadeEfs::FekBlob(aes128_key_bytes, HandMadeEfs::aes128,
                              aes128_key_bytes);
}

// One entry with the FEK given, and the first byte of its thumbprint.
HandMadeEfs::Entry EntryWith(BYTE seed, std::vector<BYTE> fek) {
  return {.seed = seed, .fek = std::move(fek)};
}

// A value of one entry with the FEK of AES-128, which change then damages.
std::vector<BYTE>
    DamagedEntry(BYTE seed,
                 const std::function<void(HandMadeEfs::Entry&)>& change) {
  HandMadeEfs::Entry entry = EntryWith(seed, Aes128Fek());
  change(entry);
  return HandMadeEfs::ValueBytes({.entries = {entry}});
}

// The encrypted volume with a $MFT of 16 clusters and files whose $EFS stream
// is damaged in one field each.
Image MalformedFields() {
  Image image = EncryptedWith(Algorithm::Aes128);
  constexpr ULONGLONG mft_clusters = 16;
  constexpr BYTE mft_cluster = 1;
  RecordEdit::MapMft(image, mft_clusters, mft_cluster,
                     FakeImage::fake_cluster_size);
  constexpr DWORD far_offset = 0x10000;
  constexpr DWORD entry_too_long = 0x1000;
  constexpr DWORD thumbprint_too_short = 0x10;
  constexpr DWORD drf_offset = 0x48;
  constexpr std::size_t header_bytes = 0x48;
  constexpr std::size_t header_and_count_bytes = 0x4C;
  constexpr ULONGLONG stream_clusters = 4;
  const std::vector<BYTE> stream_runs = {0x21, 0x04, 0x28, 0x00, 0x00};
  const std::vector<std::pair<ULONGLONG, std::vector<BYTE>>> files = {
      // The header stops before the DDF has a count.
      {7, HandMadeEfs::ValueBytes(
              {.ddf_offset = far_offset, .length = header_bytes})},
      // The value ends at the count of the DDF, which is zero.
      {10,
       HandMadeEfs::ValueBytes({.count = 0, .length = header_and_count_bytes})},
      {8, DamagedEntry(0x12, [&](auto& e) { e.length = entry_too_long; })},
      {9,
       DamagedEntry(0x13, [&](auto& e) { e.credential_offset = far_offset; })},
      {11, DamagedEntry(0x15, [&](auto& e) { e.hash_offset = far_offset; })},
      {12,
       DamagedEntry(
           0x16, [&](auto& e) { e.thumbprint_size = thumbprint_too_short; })},
      {13,
       DamagedEntry(0x17, [&](auto& e) { e.thumbprint_offset = far_offset; })},
      // A DRF with no DDF.
      {14,
       HandMadeEfs::ValueBytes({.ddf_offset = 0,
                                .drf_offset = drf_offset,
                                .entries = {EntryWith(0x11, Aes128Fek())}})},
  };
  for (const auto& [record, efs] : files) {
    PutEncryptedFile(image, {.record = record,
                             .clusters = stream_clusters,
                             .allocated_bytes =
                                 stream_clusters * FakeImage::fake_cluster_size,
                             .runs = stream_runs,
                             .efs = efs});
  }
  return image;
}

// A volume of sectors of 256 bytes, one to the cluster, whose clusters are
// smaller than the sector EFS encrypts by. The file has a $EFS of five
// entries: one good, and one for each way a FEK can be wrong.
Image UnalignedCluster() {
  constexpr WORD sector_bytes = 256;
  constexpr ULONGLONG total_sectors = 256;
  constexpr ULONGLONG mft_cluster = 4;
  constexpr DWORD clusters_per_unit = 4;
  constexpr ULONGLONG mft_clusters = 64;
  constexpr DWORD unsupported_algorithm = 0x1234;
  constexpr std::size_t short_fek_bytes = 8;
  constexpr BYTE swapped_first_marker_byte = 0xAA;
  constexpr BYTE swapped_second_marker_byte = 0x55;
  Image image = EncryptedWith(Algorithm::Aes128);
  BpbEdit::Apply(image, [&](Data::NtfsBpb& bpb) {
    bpb.bytes_per_sector = sector_bytes;
    bpb.sectors_per_cluster = 1;
    bpb.total_sectors = total_sectors;
    bpb.lcn_mft = mft_cluster;
    bpb.clusters_per_file_record = clusters_per_unit;
    bpb.clusters_per_index_block = clusters_per_unit;
    // The two bytes of the end marker are swapped, as the first recorder left
    // them.
    bpb.x_aa = swapped_first_marker_byte;
    bpb.x_55 = swapped_second_marker_byte;
  });
  RecordEdit::MapMft(image, mft_clusters, mft_cluster, sector_bytes);
  HandMadeEfs::Entry good = EntryWith(0xFF, Aes128Fek());
  good.fill = 1;
  // A FEK that stops after the key length and the second field.
  std::vector<BYTE> short_fek = Aes128Fek();
  short_fek.resize(short_fek_bytes);
  const std::vector<std::pair<BYTE, std::vector<BYTE>>> others = {
      {0x21, short_fek},
      {0x22, HandMadeEfs::FekBlob(aes128_key_bytes, unsupported_algorithm,
                                  aes128_key_bytes)},
      // The key is 32 bytes long, which AES-128 does not take.
      {0x23, HandMadeEfs::FekBlob(aes256_key_bytes, HandMadeEfs::aes128,
                                  aes256_key_bytes)},
      {0x24, HandMadeEfs::FekBlob(aes256_key_bytes, HandMadeEfs::aes256,
                                  aes256_key_bytes)}};
  HandMadeEfs::Value value{.entries = {good}};
  for (const auto& [seed, fek] : others) {
    value.entries.push_back(EntryWith(seed, fek));
  }
  constexpr ULONGLONG stream_clusters = 16;
  PutEncryptedFile(image, {.record = 6,
                           .clusters = stream_clusters,
                           .allocated_bytes = stream_clusters * sector_bytes,
                           .runs = {0x21, 0x10, 0xA0, 0x00, 0x00},
                           .efs = HandMadeEfs::ValueBytes(value)});
  return image;
}

#else

// A build without an EFS backend skips these inputs before it calls make.
enum class Algorithm : int { Aes128, Aes192, Aes256, _3Des, Desx };

Image EncryptedWith(Algorithm /*algorithm*/) {
  throw std::logic_error("this build has no EFS backend");
}

Image MalformedFields() {
  throw std::logic_error("this build has no EFS backend");
}

Image UnalignedCluster() {
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
                             .needs = Feature::Efs},
                            {.name = "efs_stream_malformed_fields",
                             .make = MalformedFields,
                             .needs = Feature::Efs},
                            {.name = "efs_encrypted_read_unaligned_cluster",
                             .make = UnalignedCluster,
                             .boot = LegacyFixtures::Boot::Current,
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
