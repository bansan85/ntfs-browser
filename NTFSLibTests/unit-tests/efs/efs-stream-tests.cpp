#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "efs-test-support.h"
#include "efs-tests-support.h"
#include "efs/efs-stream.h"
#include "efs/sector-cipher.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"
#include "test-log-sink.h"

using NTFS_BROWSER_NAMESPACE::Attr::AttrBase;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Efs::CipherBackend;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::Tests::Algorithm;
using NTFS_BROWSER_NAMESPACE::Tests::TestEfsEntry;
using NTFS_BROWSER_NAMESPACE::Tests::TestKeyProvider;

namespace {

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

// Fill of the residue past a file's initialized size.
constexpr BYTE residue_fill = 0xAB;

std::string TakeLog() {
  return NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
}

std::vector<BYTE> Slice(const std::vector<BYTE>& bytes, size_t offset,
                        size_t size) {
  const size_t end = std::min(bytes.size(), offset + size);
  return {bytes.begin() + gsl::narrow<std::ptrdiff_t>(offset),
          bytes.begin() + gsl::narrow<std::ptrdiff_t>(end)};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads back as its plaintext, whatever the cipher",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  for (const Algorithm algorithm :
       NTFS_BROWSER_NAMESPACE::Tests::all_algorithms) {
    for (const CipherBackend backend :
         {CipherBackend::CryptoPp, CipherBackend::BCrypt}) {
      if (!NTFS_BROWSER_NAMESPACE::Tests::BackendAvailable(backend)) {
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
      const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>& data =
          OnlyData<S>(*opened.record);

      const auto whole = ReadAt<S>(data, 0, fixture.plaintext.size());
      REQUIRE(whole.has_value());
      CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(whole) == fixture.plaintext);

      // A second read: decrypting in place in a shared cache would garble it.
      const auto again = ReadAt<S>(data, 0, fixture.plaintext.size());
      REQUIRE(again.has_value());
      CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(again) == fixture.plaintext);
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An unaligned read of an encrypted stream returns the plaintext slice",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const Fixture fixture = MakeFixture(Algorithm::Aes256);
  const Opened<S> opened = Open<S>(fixture.image, fixture.provider);
  const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>& data =
      OnlyData<S>(*opened.record);

  for (const size_t offset :
       {0, 1, 511, 512, 513, 1023, 1024, 1025, 2047, 2999}) {
    for (const size_t length : {1, 16, 511, 512, 513, 1500, 3000}) {
      INFO("offset " << offset << ", length " << length);
      const auto read = ReadAt<S>(data, offset, length);
      REQUIRE(read.has_value());
      CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(read) ==
            Slice(fixture.plaintext, offset, length));
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A hole inside an encrypted stream reads as zeros, not as decrypted data",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes256, key));

  // Clusters 0-1 real, 2-3 a hole, 4 real: 5000 bytes.
  constexpr size_t hole_start = 2048;
  constexpr size_t tail_start = 4096;
  const std::vector<BYTE> head =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(hole_start);
  const std::vector<BYTE> tail =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(904);

  std::vector<BYTE> cipher = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
      Algorithm::Aes256, key, head, 0);
  const std::vector<BYTE> cipher_tail =
      NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(Algorithm::Aes256, key, tail,
                                                tail_start);
  cipher.insert(cipher.end(), cipher_tail.begin(), cipher_tail.end());

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 2},
                                   {{}, 2},
                                   {first_stream_lcn + tail_extent_gap, 1}},
                          .cluster_bytes = cipher,
                          .real_size = tail_start + tail.size()});

  std::vector<BYTE> expected = head;
  expected.resize(tail_start, 0);
  expected.insert(expected.end(), tail.begin(), tail.end());

  const Opened<S> opened = Open<S>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(file),
      provider);
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, expected.size());
  REQUIRE(read.has_value());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(read) == expected);
}

TEMPLATE_TEST_CASE_SIG(
    "Every $DATA stream flagged encrypted shares the record's key, named or "
    "not",
    "[efs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes256, key));

  const std::vector<BYTE> unnamed =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(1800);
  const std::vector<BYTE> secret =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(700);
  const std::vector<BYTE> plain(300, 0x5A);

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back(
      {.runs = {{first_stream_lcn, 2}},
       .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
           Algorithm::Aes256, key, unnamed),
       .real_size = unnamed.size()});
  file.streams.push_back(
      {.name = L"secret",
       .runs = {{first_stream_lcn + secret_stream_gap, 1}},
       .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
           Algorithm::Aes256, key, secret),
       .real_size = secret.size()});
  file.streams.push_back({.name = L"plain",
                          .runs = {{first_stream_lcn + plain_stream_gap, 1}},
                          .cluster_bytes = plain,
                          .real_size = plain.size(),
                          .flagged_encrypted = false});

  Opened<S> opened = Open<S>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(file),
      provider);

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

    const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>& data =
        OnlyData<S>(*opened.record);
    CHECK_FALSE(ReadAt<S>(data, 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "Cannot decrypt the stream: no EFS key provider "
                              "is installed."));
  }

  SECTION("the provider holds another user's key") {
    const auto stranger = std::make_shared<TestKeyProvider>();
    stranger->Add(NTFS_BROWSER_NAMESPACE::Tests::TestThumbprint(stranger_seed),
                  wrapped_fek,
                  NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(
                      Algorithm::Aes256, NTFS_BROWSER_NAMESPACE::Tests::TestKey(
                                             Algorithm::Aes256)));

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
    broken->Add(NTFS_BROWSER_NAMESPACE::Tests::TestThumbprint(1), wrapped_fek,
                std::move(blob));

    (void)TakeLog();
    const Opened<S> opened = Open<S>(fixture.image, broken);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "the unwrapped FEK is unusable"));
  }

  SECTION("the record has no $EFS stream at all") {
    NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
    file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                            .cluster_bytes = std::vector<BYTE>(
                                bogus_cluster_size, bogus_cluster_fill),
                            .real_size = bogus_real_size});

    (void)TakeLog();
    const Opened<S> opened = Open<S>(
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(
            file),
        fixture.provider);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "the record has no usable $EFS stream"));
  }
}

TEMPLATE_TEST_CASE_SIG("The key can come from any entry of the $EFS stream",
                       "[efs]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes128);
  const std::vector<BYTE> plaintext =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(900);

  const std::vector<BYTE> other_wrapped(32, 0x11);
  const std::vector<BYTE> recovery_wrapped(32, 0x22);
  const std::array<TestEfsEntry, 2> users{TestUser(1, other_wrapped),
                                          TestUser(2, wrapped_fek)};
  const std::array<TestEfsEntry, 1> recovery{TestUser(3, recovery_wrapped)};

  SECTION("a later DDF entry") {
    const auto provider = std::make_shared<TestKeyProvider>();
    // users holds 2 entries.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    provider->Add(
        users[1].thumbprint, users[1].wrapped_fek,
        NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes128, key));

    NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
    file.efs_stream = NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(users);
    file.streams.push_back(
        {.runs = {{first_stream_lcn, 1}},
         .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
             Algorithm::Aes128, key, plaintext),
         .real_size = plaintext.size()});
    const Opened<S> opened = Open<S>(
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(
            file),
        provider);
    CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
  }

  SECTION("a recovery agent's entry") {
    const auto provider = std::make_shared<TestKeyProvider>();
    // recovery holds 1 entry.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    provider->Add(
        recovery[0].thumbprint, recovery[0].wrapped_fek,
        NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes128, key));

    NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
    file.efs_stream =
        NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(users, recovery);
    file.streams.push_back(
        {.runs = {{first_stream_lcn, 1}},
         .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
             Algorithm::Aes128, key, plaintext),
         .real_size = plaintext.size()});
    const Opened<S> opened = Open<S>(
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(
            file),
        provider);
    CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
  }
}

TEMPLATE_TEST_CASE_SIG("A resident $EFS stream works like a non-resident one",
                       "[efs]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256);
  const std::vector<BYTE> plaintext =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(900);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes256, key));

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_resident = true;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back(
      {.runs = {{first_stream_lcn, 1}},
       .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
           Algorithm::Aes256, key, plaintext),
       .real_size = plaintext.size()});
  const Opened<S> opened = Open<S>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(file),
      provider);
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
  provider->Add(user.thumbprint, user.wrapped_fek,
                NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(
                    Algorithm::Aes256,
                    NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256)));

  const std::vector<BYTE> on_disk(
      NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size, 0x42);

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                          .cluster_bytes = on_disk,
                          .real_size = on_disk.size(),
                          .flagged_encrypted = true,
                          .flagged_compressed = true});

  (void)TakeLog();
  const Opened<S> opened = Open<S>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(file),
      provider, std::nullopt, VolumeOptions{.recover_errors = true});

  // Never decrypted: the bytes come back exactly as they sit on disk.
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, on_disk.size());
  REQUIRE(read.has_value());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(read) == on_disk);
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
  provider->Add(user.thumbprint, user.wrapped_fek,
                NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(
                    Algorithm::Aes256,
                    NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256)));

  const std::vector<BYTE> on_disk(
      NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size, 0x42);

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, 1}},
                          .cluster_bytes = on_disk,
                          .real_size = on_disk.size(),
                          .flagged_encrypted = true,
                          .flagged_compressed = true});

  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(
              file)));
  REQUIRE(volume->IsVolumeOK());
  volume->SetEfsKeyProvider(provider);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(*volume);
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
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(size_value);
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes256, key));

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.trailing_undersized_attribute = true;
  file.streams.push_back(
      {.runs = {{first_stream_lcn, 1}},
       .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
           Algorithm::Aes256, key, plaintext),
       .real_size = size_value});

  NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(
              file)),
      VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(provider);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
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
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(size_value);
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes256, key));

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_resident = true;
  file.efs_body_overruns = true;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back(
      {.runs = {{first_stream_lcn, 1}},
       .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
           Algorithm::Aes256, key, plaintext),
       .real_size = size_value});

  NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(
              file)),
      VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());
  volume.SetEfsKeyProvider(provider);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK_FALSE(record.ParseAttrs());

  // Without its keys the stream cannot be read: it must fail, not hand back
  // the bytes as they sit on disk.
  CHECK_FALSE(ReadAt<S>(OnlyData<S>(record), 0, size_value).has_value());
}

TEMPLATE_TEST_CASE_SIG("An encrypted directory parses and lists its entries",
                       "[efs]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // The named records are not built: only recover_errors lists these entries.
  Opened<S> opened = Open<S>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedDirectory(),
      nullptr, std::nullopt, VolumeOptions{.recover_errors = true});
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
  CHECK(names[0] ==
        NTFS_BROWSER_NAMESPACE::Tests::encrypted_directory_names[0]);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] ==
        NTFS_BROWSER_NAMESPACE::Tests::encrypted_directory_names[1]);
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
  Opened<S> opened =
      Open<S>(NTFS_BROWSER_NAMESPACE::Tests::
                  BuildFakeNtfsImageWithCompressedEncryptedDirectory(),
              nullptr, std::nullopt, VolumeOptions{.recover_errors = true});
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
  CHECK(names[0] ==
        NTFS_BROWSER_NAMESPACE::Tests::encrypted_directory_names[0]);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] ==
        NTFS_BROWSER_NAMESPACE::Tests::encrypted_directory_names[1]);
}
#endif

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads as zeros beyond its initialized size",
    "[efs][regression]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Aes256);
  const TestEfsEntry user = TestUser();
  const auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(Algorithm::Aes256, key));

  constexpr size_t real_size = 3000;
  constexpr size_t ini_size = 1500;
  const std::vector<BYTE> head =
      NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(ini_size);

  // Ciphertext up to the initialized size, then residue the key never wrote.
  std::vector<BYTE> cluster = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
      Algorithm::Aes256, key, head, 0);
  cluster.resize(size_t{ClustersFor(real_size)} *
                     NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size,
                 residue_fill);

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{first_stream_lcn, ClustersFor(real_size)}},
                          .cluster_bytes = cluster,
                          .real_size = real_size,
                          .ini_size = ini_size});

  std::vector<BYTE> expected = head;
  expected.resize(real_size, 0);

  const Opened<S> opened = Open<S>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(file),
      provider);
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, real_size);
  REQUIRE(read.has_value());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(read) == expected);

  const auto tail = ReadAt<S>(OnlyData<S>(*opened.record), 2000, 500);
  REQUIRE(tail.has_value());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(tail) ==
        std::vector<BYTE>(500, 0));
}
