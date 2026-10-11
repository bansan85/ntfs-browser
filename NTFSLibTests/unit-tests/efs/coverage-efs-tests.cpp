#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "efs-test-support.h"
#include "efs/efs-stream.h"
#include "efs/fek.h"
#include "efs/sector-cipher.h"

namespace {

using NtfsBrowser::Efs::Fek;
using NtfsBrowser::Efs::sector_size;
using NtfsBrowserTests::Algorithm;
using NtfsBrowserTests::TestEfsEntry;

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
  for (const Algorithm algorithm : NtfsBrowserTests::all_algorithms) {
    INFO("algorithm 0x" << std::hex << static_cast<int>(algorithm));
    const std::vector<BYTE> key = NtfsBrowserTests::TestKey(algorithm);
    const std::optional<Fek> fek =
        Fek::Parse(NtfsBrowserTests::MakeFekBlob(algorithm, key));
    REQUIRE(fek.has_value());
    const auto decryptor = NtfsBrowser::Efs::MakeCryptoPpDecryptor(*fek);
    REQUIRE(decryptor != nullptr);

    for (const size_t size : wrong_span_sizes) {
      INFO("span size " << size);
      std::vector<BYTE> span_bytes(size, refused_fill);
      CHECK_FALSE(decryptor->DecryptSector(sector_offset, span_bytes));
      CHECK(std::ranges::all_of(
          span_bytes, [](BYTE byte) { return byte == refused_fill; }));
    }

    const std::vector<BYTE> plaintext =
        NtfsBrowserTests::PlaintextPattern(sector_size);
    std::vector<BYTE> sector =
        NtfsBrowserTests::EfsEncrypt(algorithm, key, plaintext, sector_offset);
    REQUIRE(decryptor->DecryptSector(sector_offset, sector));
    CHECK(sector == plaintext);
  }
}

#endif

TEST_CASE("An $EFS entry with no FEK bytes, or an oversized one, is rejected",
          "[cov-efs][efs]") {
  const TestEfsEntry oversized_fek{
      NtfsBrowserTests::TestThumbprint(1),
      std::vector<BYTE>(max_wrapped_fek_size + 1, oversized_fek_fill)};
  const std::vector<BYTE> oversized_stream =
      NtfsBrowserTests::MakeEfsStream(std::span(&oversized_fek, 1));
  CHECK_FALSE(NtfsBrowser::Efs::ParseEfsStream(oversized_stream).has_value());

  // MakeEfsStream() cannot write an empty FEK, so a one-byte one is forged
  // to zero: the FEK length field sits 8 bytes into the entry.
  const TestEfsEntry one_byte_fek{NtfsBrowserTests::TestThumbprint(1),
                                  std::vector<BYTE>(1, oversized_fek_fill)};
  std::vector<BYTE> empty_stream =
      NtfsBrowserTests::MakeEfsStream(std::span(&one_byte_fek, 1));
  DWORD ddf_offset = 0;
  std::memcpy(&ddf_offset, &empty_stream[ddf_offset_field], sizeof(ddf_offset));
  const size_t fek_length_field = ddf_offset + count_size + entry_fek_length;
  const DWORD zero = 0;
  std::memcpy(&empty_stream[fek_length_field], &zero, sizeof(zero));
  CHECK_FALSE(NtfsBrowser::Efs::ParseEfsStream(empty_stream).has_value());
}
