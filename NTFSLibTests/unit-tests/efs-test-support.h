#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/efs.h>

#include "efs/fek.h"

namespace NtfsBrowserTests {

// Size of a certificate thumbprint: a SHA-1 digest.
inline constexpr size_t thumbprint_size_value = 20;

using NtfsBrowser::Efs::Algorithm;

// Every algorithm EFS can use, for the tests that loop over them.
inline constexpr std::array<Algorithm, 5> all_algorithms{
    Algorithm::Aes128, Algorithm::Aes192, Algorithm::Aes256, Algorithm::_3Des,
    Algorithm::Desx};

// A fixed, arbitrary key of the length the algorithm takes.
[[nodiscard]] std::vector<BYTE> TestKey(Algorithm algorithm);

// The blob an IEfsKeyProvider hands back: the 16-byte FEK header, then the key.
[[nodiscard]] std::vector<BYTE> MakeFekBlob(Algorithm algorithm,
                                            std::span<const BYTE> key);

// Encrypts "plaintext" as EFS does: zero-padded to whole sectors, each sector
// CBC-encrypted under its own IV. "streamOffset" is where plaintext[0] sits in
// the stream. Uses Crypto++ directly, so the library's own decryption code is
// checked against an implementation it shares nothing with.
[[nodiscard]] std::vector<BYTE> EfsEncrypt(Algorithm algorithm,
                                           std::span<const BYTE> key,
                                           std::span<const BYTE> plaintext,
                                           ULONGLONG stream_offset = 0);

// A deterministic, non-repeating byte pattern, so a misplaced or garbled
// slice cannot pass for the right one.
[[nodiscard]] std::vector<BYTE> PlaintextPattern(size_t size);

// One user's entry of a synthetic $EFS stream.
struct TestEfsEntry {
  std::array<BYTE, thumbprint_size_value> thumbprint{};
  std::vector<BYTE> wrapped_fek;
};

// A thumbprint that differs per seed.
[[nodiscard]] std::array<BYTE, thumbprint_size_value> TestThumbprint(BYTE seed);

// Builds an $EFS stream in the layout of a real one: header, DDF, then DRF
// when "recovery" is not empty.
[[nodiscard]] std::vector<BYTE>
    MakeEfsStream(std::span<const TestEfsEntry> users,
                  std::span<const TestEfsEntry> recovery = {});

// Hands out one FEK blob per known (thumbprint, wrapped FEK) pair. The tests
// never go near the real certificate store.
class TestKeyProvider final : public NtfsBrowser::Efs::IEfsKeyProvider {
 public:
  void Add(const std::array<BYTE, thumbprint_size_value>& thumbprint,
           std::span<const BYTE> wrapped_fek, std::vector<BYTE> blob);

  [[nodiscard]] std::optional<std::vector<BYTE>>
      UnwrapFek(std::span<const BYTE> thumbprint,
                std::span<const BYTE> wrapped_fek) const override;

 private:
  struct Known {
    std::array<BYTE, thumbprint_size_value> thumbprint;
    std::vector<BYTE> wrapped_fek;
    std::vector<BYTE> blob;
  };

  std::vector<Known> known_;
};

// Whether this build compiled the backend in.
[[nodiscard]] constexpr bool
    BackendAvailable(NtfsBrowser::Efs::CipherBackend backend) noexcept {
#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
  constexpr bool has_crypto_pp = true;
#else
  constexpr bool has_crypto_pp = false;
#endif
#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
  constexpr bool has_b_crypt = true;
#else
  constexpr bool has_b_crypt = false;
#endif
  return backend == NtfsBrowser::Efs::CipherBackend::CryptoPp ? has_crypto_pp
                                                              : has_b_crypt;
}

}  // namespace NtfsBrowserTests
