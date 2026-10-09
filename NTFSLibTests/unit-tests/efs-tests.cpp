#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <ios>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gsl/narrow>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/efs.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "efs-test-support.h"
#include "efs/efs-stream.h"
#include "efs/fek.h"
#include "efs/sector-cipher.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"
#include "test-log-sink.h"
#include "util.h"

namespace NtfsBrowser {

template <Cache::Strategy S>
class AttrBase;

}  // namespace NtfsBrowser

using NtfsBrowser::AttrBase;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Efs::CipherBackend;
using NtfsBrowser::Efs::Fek;
using NtfsBrowser::Efs::IEfsKeyProvider;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowserTests::Algorithm;
using NtfsBrowserTests::TestEfsEntry;
using NtfsBrowserTests::TestKeyProvider;

namespace {

// The wrapped FEK the fixtures store. The test provider matches it byte for
// byte, so its content only has to be recognisable.
constexpr size_t wrapped_fek_size = 32;
constexpr BYTE wrapped_fek_fill = 0xAB;
const std::vector<BYTE> wrapped_fek(wrapped_fek_size, wrapped_fek_fill);

// Plaintext size of a default fixture file: several sectors, not a multiple
// of one.
constexpr size_t default_file_size = 3000;

// Fill that tells an unread byte from a read one.
constexpr BYTE unread_fill = 0xCC;

// Distance, in clusters, between the extents of the sparse-file fixture, and
// between the streams of the multi-stream fixture.
constexpr DWORD tail_extent_gap = 8;
constexpr DWORD secret_stream_gap = 10;
constexpr DWORD plain_stream_gap = 20;

// The thumbprint seed of a user the provider knows, but the file is not for.
constexpr BYTE stranger_seed = 9;

// A FEK blob the cipher cannot use: its size, key length, and algorithm ids.
constexpr size_t broken_blob_size = 48;
constexpr DWORD broken_key_length = 32;
constexpr DWORD broken_algorithm_id = 0x1234;
constexpr DWORD unknown_algorithm_id = 0x6611;

// Offset of the algorithm DWORD in a FEK blob, and an unsupported key length.
constexpr size_t fek_algorithm_field = 8;
constexpr DWORD wrong_key_length = 16;

// A cluster of ciphertext that is never decrypted, and the size it claims.
constexpr size_t bogus_cluster_size = 1024;
constexpr BYTE bogus_cluster_fill = 7;
constexpr ULONGLONG bogus_real_size = 1000;

// Forged DWORDs of the hostile-stream cases.
constexpr DWORD all_ones = 0xFFFFFFFF;
constexpr DWORD nearly_all_ones = 0xFFFFFFF0;
constexpr DWORD largest_signed = 0x7FFFFFFF;
constexpr DWORD huge_credential_offset = 0xFFFFFF00;
constexpr DWORD too_many_entries = 65;
constexpr DWORD oversized_thumbprint = 21;

// Lengths a stream is truncated to: inside the DDF offset field, short of the
// DDF count, and short of the first entry.
constexpr std::array<size_t, 5> truncated_lengths{0, 3, 0x47, 0x54, 0x58};

// A stream of one repeated byte, far larger than any real one.
constexpr size_t hostile_stream_size = 700;
constexpr BYTE hostile_stream_fill = 0xFF;

// The random-damage test: its seed, and how many damaged streams it parses.
constexpr unsigned damage_seed = 20'260'921;
constexpr int damage_rounds = 5000;

// The wipe test's buffer: its size and its fill.
constexpr size_t secret_size = 64;
constexpr BYTE secret_fill = 0xEE;

// Fill of the residue past a file's initialized size.
constexpr BYTE residue_fill = 0xAB;

// Where the first data stream of a fixture starts on disk, in clusters.
constexpr DWORD first_stream_lcn = 30;

// The user every fixture encrypts for.
TestEfsEntry TestUser(BYTE seed = 1, std::vector<BYTE> wrapped = wrapped_fek) {
  return {NtfsBrowserTests::TestThumbprint(seed), std::move(wrapped)};
}

DWORD ClustersFor(size_t bytes) {
  return gsl::narrow<DWORD>((bytes + NtfsBrowserTests::fake_cluster_size - 1) /
                            NtfsBrowserTests::fake_cluster_size);
}

// An encrypted file of one unnamed stream, its key provider, and the
// plaintext the stream must read back as.
struct Fixture {
  std::vector<BYTE> plaintext;
  std::vector<BYTE> image;
  std::shared_ptr<TestKeyProvider> provider;
};

Fixture MakeFixture(Algorithm algorithm, size_t size = default_file_size) {
  Fixture fixture;
  fixture.plaintext = NtfsBrowserTests::PlaintextPattern(size);
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(algorithm);

  const TestEfsEntry user = TestUser();
  fixture.provider = std::make_shared<TestKeyProvider>();
  fixture.provider->Add(user.thumbprint, user.wrapped_fek,
                        NtfsBrowserTests::MakeFekBlob(algorithm, key));

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, ClustersFor(size)}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              algorithm, key, fixture.plaintext),
                          .real_size = size});
  fixture.image = NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file);
  return fixture;
}

template <Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<FileRecord<S>> record;
};

// Opens the image and parses the root record. The provider is always set,
// null included, so that no test ever reaches the real certificate store.
// "mask" is nullopt for the default Mask::ALL; passing one exercises the
// same SetAttrMask() a caller narrowing to Mask::DATA would use. "options"
// defaults to strict; a test of a salvageable condition passes recover_errors.
// "backend" is nullopt for the volume's default.
template <Cache::Strategy S>
Opened<S> Open(std::vector<BYTE> image,
               std::shared_ptr<IEfsKeyProvider> provider,
               std::optional<Attr::Mask> mask = std::nullopt,
               const VolumeOptions& options = {},
               std::optional<CipherBackend> backend = std::nullopt) {
  Opened<S> opened;
  opened.volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)),
      options);
  REQUIRE(opened.volume->IsVolumeOK());
  opened.volume->SetEfsKeyProvider(std::move(provider));
  if (backend) {
    REQUIRE(opened.volume->SetEfsCipherBackend(*backend));
  }

  opened.record = std::make_unique<FileRecord<S>>(*opened.volume);
  if (mask) {
    opened.record->SetAttrMask(*mask);
  }
  REQUIRE(
      opened.record->ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(opened.record->ParseAttrs());
  return opened;
}

// Reads up to "size" bytes at "offset". Nullopt if ReadData() failed.
template <Cache::Strategy S>
std::optional<std::vector<BYTE>> ReadAt(const AttrBase<S>& attr,
                                        ULONGLONG offset, size_t size) {
  std::vector<BYTE> buffer(size, unread_fill);
  const std::optional<ULONGLONG> read = attr.ReadData(offset, buffer);
  if (!read) {
    return std::nullopt;
  }
  buffer.resize(gsl::narrow<size_t>(*read));
  return buffer;
}

template <Cache::Strategy S>
const AttrBase<S>& OnlyData(const FileRecord<S>& record) {
  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  return *data.front();
}

std::string TakeLog() { return NtfsBrowserTests::TakeCapturedLog(); }

std::vector<BYTE> Slice(const std::vector<BYTE>& bytes, size_t offset,
                        size_t size) {
  const size_t end = std::min(bytes.size(), offset + size);
  return {bytes.begin() + gsl::narrow<std::ptrdiff_t>(offset),
          bytes.begin() + gsl::narrow<std::ptrdiff_t>(end)};
}

void Patch32(std::vector<BYTE>& bytes, size_t offset, DWORD value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

// Offsets, in a stream MakeEfsStream() builds for one user, of the fields the
// hostile-stream cases forge.
constexpr size_t ddf_offset_field = 0x40;
constexpr size_t drf_offset_field = 0x44;
constexpr size_t ddf_count = 0x54;
constexpr size_t entry_length = 0x58;
constexpr size_t entry_credential = 0x5C;
constexpr size_t entry_fek_length = 0x60;
constexpr size_t entry_fek_offset = 0x64;
constexpr size_t credential_hash_field = 0x7C;
constexpr size_t thumbprint_offset = 0xA4;
constexpr size_t thumbprint_size_value = 0xA8;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads back as its plaintext, whatever the cipher",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  for (const Algorithm algorithm : NtfsBrowserTests::all_algorithms) {
    for (const CipherBackend backend :
         {CipherBackend::CryptoPp, CipherBackend::BCrypt}) {
      if (!NtfsBrowserTests::BackendAvailable(backend)) {
        continue;
      }
#ifndef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
      if (algorithm == Algorithm::Desx && backend == CipherBackend::BCrypt) {
        // BCrypt has no DESX. With Crypto++ compiled in, MakeDecryptor()
        // falls back to it and this combination still round-trips; without
        // it there is no decryptor to fall back to at all, so this
        // combination cannot be exercised here.
        continue;
      }
#endif
      INFO("algorithm 0x" << std::hex << static_cast<DWORD>(algorithm)
                          << ", backend " << static_cast<int>(backend));

      Fixture fixture = MakeFixture(algorithm);
      const Opened<S> opened =
          Open<S>(fixture.image, fixture.provider, std::nullopt, {}, backend);
      const AttrBase<S>& data = OnlyData<S>(*opened.record);

      const auto whole = ReadAt<S>(data, 0, fixture.plaintext.size());
      REQUIRE(whole.has_value());
      CHECK(NtfsBrowserTests::Unwrap(whole) == fixture.plaintext);

      // A second read: decrypting in place in a shared cache would garble it.
      const auto again = ReadAt<S>(data, 0, fixture.plaintext.size());
      REQUIRE(again.has_value());
      CHECK(NtfsBrowserTests::Unwrap(again) == fixture.plaintext);
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An unaligned read of an encrypted stream returns the plaintext slice",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const Fixture fixture = MakeFixture(Algorithm::Aes256);
  const Opened<S> opened = Open<S>(fixture.image, fixture.provider);
  const AttrBase<S>& data = OnlyData<S>(*opened.record);

  for (const size_t offset :
       {0, 1, 511, 512, 513, 1023, 1024, 1025, 2047, 2999}) {
    for (const size_t length : {1, 16, 511, 512, 513, 1500, 3000}) {
      INFO("offset " << offset << ", length " << length);
      const auto read = ReadAt<S>(data, offset, length);
      REQUIRE(read.has_value());
      CHECK(NtfsBrowserTests::Unwrap(read) ==
            Slice(fixture.plaintext, offset, length));
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A hole inside an encrypted stream reads as zeros, not as decrypted data",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::Aes256, key));

  // Clusters 0-1 real, 2-3 a hole, 4 real: 5000 bytes.
  constexpr size_t hole_start = 2048;
  constexpr size_t tail_start = 4096;
  const std::vector<BYTE> head = NtfsBrowserTests::PlaintextPattern(hole_start);
  const std::vector<BYTE> tail = NtfsBrowserTests::PlaintextPattern(904);

  std::vector<BYTE> cipher =
      NtfsBrowserTests::EfsEncrypt(Algorithm::Aes256, key, head, 0);
  const std::vector<BYTE> cipher_tail =
      NtfsBrowserTests::EfsEncrypt(Algorithm::Aes256, key, tail, tail_start);
  cipher.insert(cipher.end(), cipher_tail.begin(), cipher_tail.end());

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 2},
                                   {{}, 2},
                                   {first_stream_lcn + tail_extent_gap, 1}},
                          .cluster_bytes = cipher,
                          .real_size = tail_start + tail.size()});

  std::vector<BYTE> expected = head;
  expected.resize(tail_start, 0);
  expected.insert(expected.end(), tail.begin(), tail.end());

  const Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, expected.size());
  REQUIRE(read.has_value());
  CHECK(NtfsBrowserTests::Unwrap(read) == expected);
}

TEMPLATE_TEST_CASE_SIG(
    "Every $DATA stream flagged encrypted shares the record's key, named or "
    "not",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::Aes256, key));

  const std::vector<BYTE> unnamed = NtfsBrowserTests::PlaintextPattern(1800);
  const std::vector<BYTE> secret = NtfsBrowserTests::PlaintextPattern(700);
  const std::vector<BYTE> plain(300, 0x5A);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 2}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::Aes256, key, unnamed),
                          .real_size = unnamed.size()});
  file.streams.push_back({.name = L"secret",
                          .runs = {{first_stream_lcn + secret_stream_gap, 1}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::Aes256, key, secret),
                          .real_size = secret.size()});
  file.streams.push_back({.name = L"plain",
                          .runs = {{first_stream_lcn + plain_stream_gap, 1}},
                          .cluster_bytes = plain,
                          .real_size = plain.size(),
                          .flagged_encrypted = false});

  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);

  const AttrBase<S>* named_stream = opened.record->FindStream(L"secret");
  REQUIRE(named_stream != nullptr);
  CHECK(ReadAt<S>(*named_stream, 0, 700) == secret);

  const AttrBase<S>* plain_stream = opened.record->FindStream(L"plain");
  REQUIRE(plain_stream != nullptr);
  CHECK(ReadAt<S>(*plain_stream, 0, 300) == plain);

  const AttrBase<S>* unnamed_stream = opened.record->FindStream(L"");
  REQUIRE(unnamed_stream != nullptr);
  CHECK(ReadAt<S>(*unnamed_stream, 0, 1800) == unnamed);
}

TEMPLATE_TEST_CASE_SIG(
    "A stream nobody holds a key for reads as nullopt, and the parse survives",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const Fixture fixture = MakeFixture(Algorithm::Aes256);

  SECTION("no provider is installed") {
    (void)TakeLog();
    Opened<S> opened = Open<S>(fixture.image, nullptr);
    CHECK(opened.record->IsEncrypted());

    const AttrBase<S>& data = OnlyData<S>(*opened.record);
    CHECK_FALSE(ReadAt<S>(data, 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "Cannot decrypt the stream: no EFS key provider "
                              "is installed."));
  }

  SECTION("the provider holds another user's key") {
    const auto stranger = std::make_shared<TestKeyProvider>();
    stranger->Add(
        NtfsBrowserTests::TestThumbprint(stranger_seed), wrapped_fek,
        NtfsBrowserTests::MakeFekBlob(
            Algorithm::Aes256, NtfsBrowserTests::TestKey(Algorithm::Aes256)));

    (void)TakeLog();
    const Opened<S> opened = Open<S>(fixture.image, stranger);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "no key provider holds a key for this file"));
  }

  SECTION("the unwrapped FEK is unusable") {
    const auto broken = std::make_shared<TestKeyProvider>();
    std::vector<BYTE> blob(broken_blob_size, 0);
    Patch32(blob, 0, broken_key_length);
    Patch32(blob, fek_algorithm_field, broken_algorithm_id);
    broken->Add(NtfsBrowserTests::TestThumbprint(1), wrapped_fek,
                std::move(blob));

    (void)TakeLog();
    const Opened<S> opened = Open<S>(fixture.image, broken);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "the unwrapped FEK is unusable"));
  }

  SECTION("the record has no $EFS stream at all") {
    NtfsBrowserTests::FakeEncryptedFile file;
    file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                            .cluster_bytes = std::vector<BYTE>(
                                bogus_cluster_size, bogus_cluster_fill),
                            .real_size = bogus_real_size});

    (void)TakeLog();
    const Opened<S> opened =
        Open<S>(NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file),
                fixture.provider);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "the record has no usable $EFS stream"));
  }
}

TEMPLATE_TEST_CASE_SIG("The key can come from any entry of the $EFS stream",
                       "[efs]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::Aes128);
  const std::vector<BYTE> plaintext = NtfsBrowserTests::PlaintextPattern(900);

  const std::vector<BYTE> other_wrapped(32, 0x11);
  const std::vector<BYTE> recovery_wrapped(32, 0x22);
  const std::array<TestEfsEntry, 2> users{TestUser(1, other_wrapped),
                                          TestUser(2, wrapped_fek)};
  const std::array<TestEfsEntry, 1> recovery{TestUser(3, recovery_wrapped)};

  SECTION("a later DDF entry") {
    const auto provider = std::make_shared<TestKeyProvider>();
    // users holds 2 entries.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    provider->Add(users[1].thumbprint, users[1].wrapped_fek,
                  NtfsBrowserTests::MakeFekBlob(Algorithm::Aes128, key));

    NtfsBrowserTests::FakeEncryptedFile file;
    file.efs_stream = NtfsBrowserTests::MakeEfsStream(users);
    file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                            .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                                Algorithm::Aes128, key, plaintext),
                            .real_size = plaintext.size()});
    const Opened<S> opened = Open<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
    CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
  }

  SECTION("a recovery agent's entry") {
    const auto provider = std::make_shared<TestKeyProvider>();
    // recovery holds 1 entry.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    provider->Add(recovery[0].thumbprint, recovery[0].wrapped_fek,
                  NtfsBrowserTests::MakeFekBlob(Algorithm::Aes128, key));

    NtfsBrowserTests::FakeEncryptedFile file;
    file.efs_stream = NtfsBrowserTests::MakeEfsStream(users, recovery);
    file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                            .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                                Algorithm::Aes128, key, plaintext),
                            .real_size = plaintext.size()});
    const Opened<S> opened = Open<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
    CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
  }
}

TEMPLATE_TEST_CASE_SIG("A resident $EFS stream works like a non-resident one",
                       "[efs]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::Aes256);
  const std::vector<BYTE> plaintext = NtfsBrowserTests::PlaintextPattern(900);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::Aes256, key));

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_resident = true;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::Aes256, key, plaintext),
                          .real_size = plaintext.size()});
  const Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
  CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
}

TEMPLATE_TEST_CASE_SIG(
    "SetAttrMask(Mask::DATA) still pulls in the $EFS stream it needs", "[efs]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // The exact mask ntfsdump/ntfsundel narrow to before reading file data.
  Fixture fixture = MakeFixture(Algorithm::Aes256);
  const Opened<S> opened =
      Open<S>(fixture.image, fixture.provider, Attr::Mask::Data);
  CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, fixture.plaintext.size()) ==
        fixture.plaintext);
}

TEMPLATE_TEST_CASE_SIG(
    "A $DATA stream flagged both compressed and encrypted is left "
    "undecrypted when recovering",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NtfsBrowserTests::MakeFekBlob(
          Algorithm::Aes256, NtfsBrowserTests::TestKey(Algorithm::Aes256)));

  const std::vector<BYTE> on_disk(NtfsBrowserTests::fake_cluster_size, 0x42);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                          .cluster_bytes = on_disk,
                          .real_size = on_disk.size(),
                          .flagged_encrypted = true,
                          .flagged_compressed = true});

  (void)TakeLog();
  const Opened<S> opened =
      Open<S>(NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file),
              provider, std::nullopt, VolumeOptions{.recover_errors = true});

  // Never decrypted: the bytes come back exactly as they sit on disk.
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, on_disk.size());
  REQUIRE(read.has_value());
  CHECK(NtfsBrowserTests::Unwrap(read) == on_disk);
  CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                            "flagged both compressed and encrypted"));
}

TEMPLATE_TEST_CASE_SIG(
    "A $DATA stream flagged both compressed and encrypted rejects the "
    "record by default",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NtfsBrowserTests::MakeFekBlob(
          Algorithm::Aes256, NtfsBrowserTests::TestKey(Algorithm::Aes256)));

  const std::vector<BYTE> on_disk(NtfsBrowserTests::fake_cluster_size, 0x42);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                          .cluster_bytes = on_disk,
                          .real_size = on_disk.size(),
                          .flagged_encrypted = true,
                          .flagged_compressed = true});

  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)));
  REQUIRE(volume->IsVolumeOK());
  volume->SetEfsKeyProvider(provider);

  FileRecord<S> record(*volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::Data).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "A recovering parse that stops on a malformed attribute still decrypts "
    "the stream",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // Less than one 1024-byte cluster: one encrypted sector run.
  constexpr size_t size_value = 1000;
  const std::vector<BYTE> plaintext =
      NtfsBrowserTests::PlaintextPattern(size_value);
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::Aes256, key));

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.trailing_undersized_attribute = true;
  file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::Aes256, key, plaintext),
                          .real_size = size_value});

  NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)),
      VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(provider);

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK_FALSE(record.ParseAttrs());

  CHECK(ReadAt<S>(OnlyData<S>(record), 0, size_value) == plaintext);
}

TEMPLATE_TEST_CASE_SIG(
    "A recovering parse that rejects the $EFS stream does not read the "
    "ciphertext back as data",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // Less than one 1024-byte cluster: one encrypted sector run.
  constexpr size_t size_value = 1000;
  const std::vector<BYTE> plaintext =
      NtfsBrowserTests::PlaintextPattern(size_value);
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::Aes256, key));

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_resident = true;
  file.efs_body_overruns = true;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::Aes256, key, plaintext),
                          .real_size = size_value});

  NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)),
      VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(provider);

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK_FALSE(record.ParseAttrs());

  // Without its keys the stream cannot be read: it must fail, not hand back
  // the bytes as they sit on disk.
  CHECK_FALSE(ReadAt<S>(OnlyData<S>(record), 0, size_value).has_value());
}

TEMPLATE_TEST_CASE_SIG("An encrypted directory parses and lists its entries",
                       "[efs]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedDirectory(), nullptr);
  CHECK(opened.record->IsEncrypted());
  CHECK(opened.record->IsDirectory());

  std::vector<std::wstring> names;
  opened.record->TraverseSubEntries(
      [](const IndexEntryView& entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            entry.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == NtfsBrowserTests::encrypted_directory_names[0]);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] == NtfsBrowserTests::encrypted_directory_names[1]);
}

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
// This test's whole premise is a compressed $INDEX_ALLOCATION: with
// decompression not compiled in, that attribute is rejected on sight
// (see attr-non-resident.cpp), so the record never parses at all. There is
// no meaningful compressed-and-encrypted-directory case left to check then,
// the same way attr-compression-tests.cpp is excluded outright.
TEMPLATE_TEST_CASE_SIG(
    "A compressed $INDEX_ALLOCATION still lists under an encrypted directory",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCompressedEncryptedDirectory(),
      nullptr);
  CHECK(opened.record->IsEncrypted());
  CHECK(opened.record->IsCompressed());
  CHECK(opened.record->IsDirectory());

  std::vector<std::wstring> names;
  opened.record->TraverseSubEntries(
      [](const IndexEntryView& entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            entry.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == NtfsBrowserTests::encrypted_directory_names[0]);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] == NtfsBrowserTests::encrypted_directory_names[1]);
}
#endif

TEMPLATE_TEST_CASE_SIG(
    "A hostile $EFS stream never crashes the parse and never yields a key",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const TestEfsEntry user = TestUser();
  const std::vector<BYTE> valid =
      NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  REQUIRE(NtfsBrowser::Efs::ParseEfsStream(valid).has_value());

  std::vector<std::vector<BYTE>> hostile;
  const auto forge = [&](size_t offset, DWORD value) {
    std::vector<BYTE> copy = valid;
    Patch32(copy, offset, value);
    hostile.push_back(std::move(copy));
  };
  forge(ddf_offset_field, nearly_all_ones);
  forge(ddf_offset_field, gsl::narrow<DWORD>(valid.size()));
  forge(drf_offset_field, largest_signed);
  forge(ddf_count, all_ones);
  forge(ddf_count, too_many_entries);
  forge(ddf_count, 2);
  forge(entry_length, 0);
  forge(entry_length, all_ones);
  forge(entry_credential, huge_credential_offset);
  forge(entry_fek_length, all_ones);
  forge(entry_fek_length, 0);
  forge(entry_fek_offset, all_ones);
  forge(credential_hash_field, all_ones);
  forge(thumbprint_offset, all_ones);
  forge(thumbprint_size_value, oversized_thumbprint);
  forge(thumbprint_size_value, all_ones);
  std::vector<size_t> lengths(truncated_lengths.begin(),
                              truncated_lengths.end());
  lengths.push_back(valid.size() - 1);
  for (const size_t length : lengths) {
    hostile.emplace_back(valid.begin(),
                         valid.begin() + gsl::narrow<std::ptrdiff_t>(length));
  }
  hostile.emplace_back(hostile_stream_size, hostile_stream_fill);

  const Fixture fixture = MakeFixture(Algorithm::Aes256, bogus_real_size);
  for (const std::vector<BYTE>& stream : hostile) {
    INFO("stream of " << stream.size() << " bytes");
    CHECK_FALSE(NtfsBrowser::Efs::ParseEfsStream(stream).has_value());

    NtfsBrowserTests::FakeEncryptedFile file;
    file.efs_stream = stream;
    file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                            .cluster_bytes = std::vector<BYTE>(
                                bogus_cluster_size, bogus_cluster_fill),
                            .real_size = bogus_real_size});
    if (stream.empty()) {
      continue;
    }
    const Opened<S> opened =
        Open<S>(NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file),
                fixture.provider);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
  }
}

TEST_CASE("A $EFS stream with random damage never crashes the parser",
          "[efs]") {
  const std::array<TestEfsEntry, 2> users{TestUser(1), TestUser(2)};
  const std::vector<BYTE> valid = NtfsBrowserTests::MakeEfsStream(users, users);

  // A fixed seed keeps the damage reproducible.
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937 random(damage_seed);
  for (int round = 0; round < damage_rounds; ++round) {
    std::vector<BYTE> damaged = valid;
    const int flips = 1 + static_cast<int>(random() % 4);
    for (int i = 0; i < flips; ++i) {
      // The index is reduced modulo damaged.size().
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      damaged[random() % damaged.size()] = static_cast<BYTE>(random());
    }
    if ((random() % 4) == 0) {
      damaged.resize(random() % damaged.size());
    }
    (void)NtfsBrowser::Efs::ParseEfsStream(damaged);
  }
  SUCCEED();
}

TEST_CASE("The $EFS parser reads the users out of a well-formed stream",
          "[efs]") {
  const std::array<TestEfsEntry, 2> users{TestUser(1, std::vector<BYTE>(8, 1)),
                                          TestUser(2, std::vector<BYTE>(9, 2))};
  const std::array<TestEfsEntry, 1> recovery{
      TestUser(3, std::vector<BYTE>(10, 3))};

  const auto parsed = NtfsBrowser::Efs::ParseEfsStream(
      NtfsBrowserTests::MakeEfsStream(users, recovery));
  REQUIRE(parsed.has_value());
  REQUIRE(NtfsBrowserTests::Unwrap(parsed).size() == 3);

  for (size_t i = 0; i < 2; ++i) {
    // i < 2 = users.size(), and the REQUIRE above checks that parsed holds 3.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(std::equal(
        NtfsBrowserTests::Unwrap(parsed)[i].thumbprint.begin(),
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        NtfsBrowserTests::Unwrap(parsed)[i].thumbprint.end(),
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        users[i].thumbprint.begin()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(NtfsBrowserTests::Unwrap(parsed)[i].wrapped_fek ==
          users[i].wrapped_fek);
  }
  // The REQUIRE above checks that parsed holds 3; recovery holds 1.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(NtfsBrowserTests::Unwrap(parsed)[2].wrapped_fek ==
        recovery[0].wrapped_fek);

  CHECK(NtfsBrowserTests::Unwrap(NtfsBrowser::Efs::ParseEfsStream(
                                     NtfsBrowserTests::MakeEfsStream({}, {})))
            .empty());
}

TEST_CASE("A FEK blob is checked against its algorithm", "[efs]") {
  for (const Algorithm algorithm : NtfsBrowserTests::all_algorithms) {
    const std::vector<BYTE> key = NtfsBrowserTests::TestKey(algorithm);
    const std::vector<BYTE> blob =
        NtfsBrowserTests::MakeFekBlob(algorithm, key);

    const std::optional<Fek> fek = Fek::Parse(blob);
    REQUIRE(fek.has_value());
    CHECK(NtfsBrowserTests::Unwrap(fek).GetAlgorithm() == algorithm);
    CHECK(std::equal(NtfsBrowserTests::Unwrap(fek).GetKey().begin(),
                     NtfsBrowserTests::Unwrap(fek).GetKey().end(), key.begin(),
                     key.end()));
  }

  const std::vector<BYTE> good = NtfsBrowserTests::MakeFekBlob(
      Algorithm::Aes256, NtfsBrowserTests::TestKey(Algorithm::Aes256));

  CHECK_FALSE(Fek::Parse({}).has_value());
  CHECK_FALSE(Fek::Parse(std::span(good).first(15)).has_value());
  CHECK_FALSE(Fek::Parse(std::span(good).first(good.size() - 1)).has_value());

  std::vector<BYTE> unknown = good;
  Patch32(unknown, fek_algorithm_field, unknown_algorithm_id);
  CHECK_FALSE(Fek::Parse(unknown).has_value());

  std::vector<BYTE> wrong_length = good;
  Patch32(wrong_length, 0, wrong_key_length);
  CHECK_FALSE(Fek::Parse(wrong_length).has_value());

  std::vector<BYTE> huge_length = good;
  Patch32(huge_length, 0, all_ones);
  CHECK_FALSE(Fek::Parse(huge_length).has_value());
}

TEST_CASE("A DESX FEK carries a 16-byte key, not a 24-byte one", "[efs]") {
  const std::vector<BYTE> sixteen(16, 0x42);
  const std::vector<BYTE> twenty_four(24, 0x42);

  const std::optional<Fek> fek =
      Fek::Parse(NtfsBrowserTests::MakeFekBlob(Algorithm::Desx, sixteen));
  REQUIRE(fek.has_value());
  CHECK(NtfsBrowserTests::Unwrap(fek).GetKey().size() == 16);

  CHECK_FALSE(
      Fek::Parse(NtfsBrowserTests::MakeFekBlob(Algorithm::Desx, twenty_four))
          .has_value());
}

TEST_CASE("Secure zero wipes what it is given", "[efs]") {
  std::vector<BYTE> secret(secret_size, secret_fill);
  NtfsBrowser::Util::SecureZero(secret);
  CHECK(std::ranges::all_of(secret, [](BYTE byte) { return byte == 0; }));
}

TEST_CASE("The cipher backend is selectable per volume, Crypto++ by default",
          "[efs]") {
  const Fixture fixture = MakeFixture(Algorithm::Aes256);
  const Opened<Cache::Strategy::NoCache> opened =
      Open<Cache::Strategy::NoCache>(fixture.image, fixture.provider);
  NtfsVolume<Cache::Strategy::NoCache>& volume = *opened.volume;

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
  CHECK(volume.GetEfsCipherBackend() == CipherBackend::CryptoPp);
#else
  // Crypto++ is not compiled in: the file only builds at all because BCrypt
  // is, so that is the default instead.
  CHECK(volume.GetEfsCipherBackend() == CipherBackend::BCrypt);
#endif

  for (const CipherBackend backend :
       {CipherBackend::CryptoPp, CipherBackend::BCrypt}) {
    const CipherBackend before = volume.GetEfsCipherBackend();
    CHECK(volume.SetEfsCipherBackend(backend) ==
          NtfsBrowserTests::BackendAvailable(backend));
    CHECK(volume.GetEfsCipherBackend() ==
          (NtfsBrowserTests::BackendAvailable(backend) ? backend : before));
  }
}

namespace {

// One published block-cipher known-answer: the first block of the sector's
// ciphertext, and the plaintext block it decrypts to under a zero-based IV.
// "sector_zero_iv" is the IV EFS uses for sector 0 with that cipher.
struct KnownAnswer {
  Algorithm algorithm;
  std::vector<BYTE> key;
  std::vector<BYTE> ciphertext_block;
  std::vector<BYTE> plaintext_block;
  std::vector<BYTE> sector_zero_iv;
};

std::vector<BYTE> Bytes(std::initializer_list<int> values) {
  return {values.begin(), values.end()};
}

std::vector<BYTE> Counting(size_t length) {
  std::vector<BYTE> bytes(length);
  for (size_t i = 0; i < length; ++i) {
    // i < bytes.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[i] = gsl::narrow<BYTE>(i);
  }
  return bytes;
}

// The IV bytes of sector 0 for the AES family, as read off a real AES-256 EFS
// file.
const std::vector<BYTE> aes_sector_zero_iv =
    Bytes({0x12, 0x13, 0x16, 0xE9, 0x7B, 0x65, 0x16, 0x58, 0x61, 0x89, 0x91,
           0x44, 0xBE, 0xAD, 0x89, 0x19});

// The IV bytes of sector 0 for the DES family: the single little-endian word
// 0x169119629891ad13 that ntfs-3g applies to DES, 3DES and DESX.
const std::vector<BYTE> des_sector_zero_iv =
    Bytes({0x13, 0xAD, 0x91, 0x98, 0x62, 0x19, 0x91, 0x16});

}  // namespace

TEST_CASE("Sector decryption matches the published block-cipher vectors",
          "[efs]") {
  // FIPS-197 appendix C for AES. For 3DES, the FIPS 81 vector: with all three
  // keys equal, 3DES is single DES.
  const std::vector<KnownAnswer> answers{
      {Algorithm::Aes128, Counting(16),
       Bytes({0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30, 0xD8, 0xCD, 0xB7,
              0x80, 0x70, 0xB4, 0xC5, 0x5A}),
       Bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
              0xBB, 0xCC, 0xDD, 0xEE, 0xFF}),
       aes_sector_zero_iv},
      {Algorithm::Aes192, Counting(24),
       Bytes({0xDD, 0xA9, 0x7C, 0xA4, 0x86, 0x4C, 0xDF, 0xE0, 0x6E, 0xAF, 0x70,
              0xA0, 0xEC, 0x0D, 0x71, 0x91}),
       Bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
              0xBB, 0xCC, 0xDD, 0xEE, 0xFF}),
       aes_sector_zero_iv},
      {Algorithm::Aes256, Counting(32),
       Bytes({0x8E, 0xA2, 0xB7, 0xCA, 0x51, 0x67, 0x45, 0xBF, 0xEA, 0xFC, 0x49,
              0x90, 0x4B, 0x49, 0x60, 0x89}),
       Bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
              0xBB, 0xCC, 0xDD, 0xEE, 0xFF}),
       aes_sector_zero_iv},
      {Algorithm::_3Des,
       Bytes({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
              0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
              0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}),
       Bytes({0x3F, 0xA4, 0x0E, 0x8A, 0x98, 0x4D, 0x48, 0x15}),
       Bytes({0x4E, 0x6F, 0x77, 0x20, 0x69, 0x73, 0x20, 0x74}),
       des_sector_zero_iv}};

  for (const KnownAnswer& answer : answers) {
    for (const CipherBackend backend :
         {CipherBackend::CryptoPp, CipherBackend::BCrypt}) {
      if (!NtfsBrowserTests::BackendAvailable(backend)) {
        continue;
      }
      INFO("algorithm 0x" << std::hex << static_cast<DWORD>(answer.algorithm)
                          << ", backend " << static_cast<int>(backend));

      // CBC: the first block decrypts to (block cipher output) xor IV. So a
      // ciphertext block that is the published one must give plaintext xor IV.
      const std::vector<BYTE> blob =
          NtfsBrowserTests::MakeFekBlob(answer.algorithm, answer.key);
      const std::optional<Fek> fek = Fek::Parse(blob);
      REQUIRE(fek.has_value());

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
      auto decryptor = NtfsBrowser::Efs::MakeCryptoPpDecryptor(
          NtfsBrowserTests::Unwrap(fek));
#else
      std::unique_ptr<NtfsBrowser::Efs::SectorDecryptor> decryptor;
#endif
#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
      if (backend == CipherBackend::BCrypt) {
        decryptor = NtfsBrowser::Efs::MakeBCryptDecryptor(*fek);
      }
#endif
      REQUIRE(decryptor != nullptr);

      std::vector<BYTE> sector(NtfsBrowser::Efs::sector_size, 0);
      std::ranges::copy(answer.ciphertext_block, sector.begin());
      REQUIRE(decryptor->DecryptSector(0, sector));

      for (size_t i = 0; i < answer.plaintext_block.size(); ++i) {
        // i < answer.plaintext_block.size() by the loop condition.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        CHECK(sector.at(i) == static_cast<BYTE>(answer.plaintext_block[i] ^
                                                answer.sector_zero_iv.at(i)));
      }
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads as zeros beyond its initialized size",
    "[efs][regression]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::Aes256, key));

  constexpr size_t real_size = 3000;
  constexpr size_t ini_size = 1500;
  const std::vector<BYTE> head = NtfsBrowserTests::PlaintextPattern(ini_size);

  // Ciphertext up to the initialized size, then residue the key never wrote.
  std::vector<BYTE> cluster =
      NtfsBrowserTests::EfsEncrypt(Algorithm::Aes256, key, head, 0);
  cluster.resize(size_t{ClustersFor(real_size)} *
                     NtfsBrowserTests::fake_cluster_size,
                 residue_fill);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, ClustersFor(real_size)}},
                          .cluster_bytes = cluster,
                          .real_size = real_size,
                          .ini_size = ini_size});

  std::vector<BYTE> expected = head;
  expected.resize(real_size, 0);

  const Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, real_size);
  REQUIRE(read.has_value());
  CHECK(NtfsBrowserTests::Unwrap(read) == expected);

  const auto tail = ReadAt<S>(OnlyData<S>(*opened.record), 2000, 500);
  REQUIRE(tail.has_value());
  CHECK(NtfsBrowserTests::Unwrap(tail) == std::vector<BYTE>(500, 0));
}
