#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "efs-test-support.h"
#include "efs/efs-stream.h"
#include "efs/fek.h"
#include "efs/sector-cipher.h"

namespace {

using NTFS_BROWSER_NAMESPACE::Efs::Fek;
using NTFS_BROWSER_NAMESPACE::Efs::sector_size;
using NTFS_BROWSER_NAMESPACE::Tests::Algorithm;
using NTFS_BROWSER_NAMESPACE::Tests::TestEfsEntry;

// Stream offset of the sector under test. Non-zero, so the IV is not the one
// of the first sector.
constexpr ULONGLONG sector_offset = 3 * sector_size;

// Spans of the wrong length: empty, one byte short and long, and two sectors.
constexpr std::array<size_t, 4> wrong_span_sizes{
    0, sector_size - 1, sector_size + 1, 2 * sector_size};

// Fill of the refused spans. A decryptor that wrote into one would show.
constexpr BYTE refused_fill = 0x5A;

// Largest wrapped FEK the $EFS parser keeps, in bytes. One byte more is
// rejected. Mirrors the limit in efs-stream.cpp.
constexpr size_t max_wrapped_fek_size = 1024;

// Fill of the wrapped FEK bytes in the oversized entry. Any value works.
constexpr BYTE oversized_fek_fill = 0xAB;

// Offsets of the DDF pointer in a $EFS header, the DWORD count that opens the
// DDF, and the FEK length field of its first entry. Match efs-stream.cpp.
constexpr size_t ddf_offset_field = 0x40;
constexpr size_t count_size = sizeof(DWORD);
constexpr size_t entry_fek_length = 0x08;

}  // namespace

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP

TEST_CASE("A Crypto++ sector decryptor takes exactly one sector",
          "[cov-efs][efs]") {
  for (const Algorithm algorithm :
       NTFS_BROWSER_NAMESPACE::Tests::all_algorithms) {
    INFO("algorithm 0x" << std::hex << static_cast<int>(algorithm));
    const std::vector<BYTE> key =
        NTFS_BROWSER_NAMESPACE::Tests::TestKey(algorithm);
    const std::optional<Fek> fek =
        Fek::Parse(NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(algorithm, key));
    REQUIRE(fek.has_value());
    const auto decryptor =
        NTFS_BROWSER_NAMESPACE::Efs::MakeCryptoPpDecryptor(*fek);
    REQUIRE(decryptor != nullptr);

    for (const size_t size : wrong_span_sizes) {
      INFO("span size " << size);
      std::vector<BYTE> span_bytes(size, refused_fill);
      CHECK_FALSE(decryptor->DecryptSector(sector_offset, span_bytes));
      CHECK(std::ranges::all_of(
          span_bytes, [](BYTE byte) { return byte == refused_fill; }));
    }

    const std::vector<BYTE> plaintext =
        NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(sector_size);
    std::vector<BYTE> sector = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
        algorithm, key, plaintext, sector_offset);
    REQUIRE(decryptor->DecryptSector(sector_offset, sector));
    CHECK(sector == plaintext);
  }
}

#endif

#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)

TEST_CASE("A BCrypt sector decryptor takes exactly one sector",
          "[cov-efs][efs]") {
  // BCrypt has no DESX, so its decryptor covers the other four algorithms.
  for (const Algorithm algorithm : {Algorithm::Aes128, Algorithm::Aes192,
                                    Algorithm::Aes256, Algorithm::_3Des}) {
    INFO("algorithm 0x" << std::hex << static_cast<int>(algorithm));
    const std::vector<BYTE> key =
        NTFS_BROWSER_NAMESPACE::Tests::TestKey(algorithm);
    const std::optional<Fek> fek =
        Fek::Parse(NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(algorithm, key));
    REQUIRE(fek.has_value());
    const auto decryptor =
        NTFS_BROWSER_NAMESPACE::Efs::MakeBCryptDecryptor(*fek);
    REQUIRE(decryptor != nullptr);

    for (const size_t size : wrong_span_sizes) {
      INFO("span size " << size);
      std::vector<BYTE> span_bytes(size, refused_fill);
      CHECK_FALSE(decryptor->DecryptSector(sector_offset, span_bytes));
      CHECK(std::ranges::all_of(
          span_bytes, [](BYTE byte) { return byte == refused_fill; }));
    }

    const std::vector<BYTE> plaintext =
        NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(sector_size);
    std::vector<BYTE> sector = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
        algorithm, key, plaintext, sector_offset);
    REQUIRE(decryptor->DecryptSector(sector_offset, sector));
    CHECK(sector == plaintext);
  }
}

TEST_CASE("The BCrypt backend declines a DESX key", "[cov-efs][efs]") {
  const std::optional<Fek> fek =
      Fek::Parse(NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(
          Algorithm::Desx,
          NTFS_BROWSER_NAMESPACE::Tests::TestKey(Algorithm::Desx)));
  REQUIRE(fek.has_value());

  CHECK(NTFS_BROWSER_NAMESPACE::Efs::MakeBCryptDecryptor(*fek) == nullptr);
}

#endif

TEST_CASE("An $EFS entry with no FEK bytes, or an oversized one, is rejected",
          "[cov-efs][efs]") {
  const TestEfsEntry oversized_fek{
      NTFS_BROWSER_NAMESPACE::Tests::TestThumbprint(1),
      std::vector<BYTE>(max_wrapped_fek_size + 1, oversized_fek_fill)};
  const std::vector<BYTE> oversized_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(
          std::span(&oversized_fek, 1));
  CHECK_FALSE(NTFS_BROWSER_NAMESPACE::Efs::ParseEfsStream(oversized_stream)
                  .has_value());

  // MakeEfsStream() cannot write an empty FEK, so a one-byte one is forged
  // to zero: the FEK length field sits 8 bytes into the entry.
  const TestEfsEntry one_byte_fek{
      NTFS_BROWSER_NAMESPACE::Tests::TestThumbprint(1),
      std::vector<BYTE>(1, oversized_fek_fill)};
  std::vector<BYTE> empty_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&one_byte_fek, 1));
  DWORD ddf_offset = 0;
  std::memcpy(&ddf_offset, &empty_stream[ddf_offset_field], sizeof(ddf_offset));
  const size_t fek_length_field = ddf_offset + count_size + entry_fek_length;
  const DWORD zero = 0;
  std::memcpy(&empty_stream[fek_length_field], &zero, sizeof(zero));
  CHECK_FALSE(
      NTFS_BROWSER_NAMESPACE::Efs::ParseEfsStream(empty_stream).has_value());
}
