#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ios>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "catch2/catch_message.hpp"
#include "efs-test-support.h"
#include "efs-tests-support.h"
#include "efs/efs-stream.h"
#include "efs/fek.h"
#include "efs/sector-cipher.h"
#include "fake-ntfs-image.h"
#include "optional-access.h"
#include "util/util.h"

using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::Efs::CipherBackend;
using NtfsBrowser::Efs::Fek;
using NtfsBrowserTests::Algorithm;
using NtfsBrowserTests::TestEfsEntry;

namespace {

constexpr DWORD unknown_algorithm_id = 0x6611;

constexpr DWORD wrong_key_length = 16;

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
