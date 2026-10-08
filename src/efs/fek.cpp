#include "efs/fek.h"

#include <ntfs-browser/win-types.h>

#include <cstring>

#include "ntfs-common.h"

namespace NtfsBrowser::Efs {

namespace {

// Size of the FEK blob header: key length, entropy, algorithm, reserved. Four
// DWORDs, which is how the blob is laid out on disk.
constexpr size_t fek_header_size = 16;

// Offset of the algorithm DWORD in the FEK header.
constexpr size_t algorithm_offset = 8;

// Key sizes in bytes. DESX takes 128 bits that the cipher expands.
constexpr size_t aes128_key_size = 16;
constexpr size_t aes192_key_size = 24;
constexpr size_t aes256_key_size = 32;
constexpr size_t _3_des_key_size = 24;
constexpr size_t desx_fek_key_size = 16;

// The key length each cipher takes, in bytes. 3DES carries three 8-byte DES
// keys. DESX carries 128 bits, which the cipher expands into a DES key and two
// whitening keys.
[[nodiscard]] std::optional<size_t> KeyLengthOf(Algorithm algorithm) noexcept {
  switch (algorithm) {
    case Algorithm::Aes128:
      return aes128_key_size;
    case Algorithm::Aes192:
      return aes192_key_size;
    case Algorithm::Aes256:
      return aes256_key_size;
    case Algorithm::_3Des:
      return _3_des_key_size;
    case Algorithm::Desx:
      return desx_fek_key_size;
  }
  return std::nullopt;
}

// Reads the DWORD at "offset". The caller checked that it lies in "bytes".
[[nodiscard]] DWORD LoadDword(std::span<const BYTE> bytes,
                              size_t offset) noexcept {
  DWORD value = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  std::memcpy(&value, &bytes[offset], sizeof(value));
  return value;
}

}  // namespace

void SecureZero(std::span<BYTE> bytes) noexcept {
  // The volatile write keeps the compiler from eliding the wipe.
  for (BYTE& byte : bytes)  // NOLINT(misc-const-correctness)
  {
    *static_cast<volatile BYTE*>(&byte) = 0;
  }
}

Fek::Fek(Algorithm algorithm, std::span<const BYTE> key)
    : algorithm_(algorithm), key_(key.begin(), key.end()) {}

Fek::~Fek() { SecureZero(key_); }

std::optional<Fek> Fek::Parse(std::span<const BYTE> blob) {
  if (blob.size() < fek_header_size) {
    LogWarn("FEK blob is too short: {} bytes.", blob.size());
    return std::nullopt;
  }

  const auto algorithm =
      static_cast<Algorithm>(LoadDword(blob, algorithm_offset));
  const std::optional<size_t> key_length = KeyLengthOf(algorithm);
  if (!key_length) {
    LogWarn("Unsupported EFS algorithm: 0x{:04X}.",
            static_cast<DWORD>(algorithm));
    return std::nullopt;
  }

  // The declared key length must be the one the cipher takes, and the key
  // must fit in the blob.
  if (LoadDword(blob, 0) != *key_length ||
      blob.size() - fek_header_size < *key_length) {
    LogWarn("FEK key length does not match algorithm 0x{:04X}.",
            static_cast<DWORD>(algorithm));
    return std::nullopt;
  }

  return Fek(algorithm, blob.subspan(fek_header_size, *key_length));
}

Algorithm Fek::GetAlgorithm() const noexcept { return algorithm_; }

std::span<const BYTE> Fek::GetKey() const noexcept { return key_; }

}  // namespace NtfsBrowser::Efs
