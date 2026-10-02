#include "efs/fek.h"

#include <ntfs-browser/win-types.h>

#include <cstring>

#include "ntfs-common.h"

namespace NtfsBrowser::Efs
{

namespace
{
// Size of the FEK blob header: key length, entropy, algorithm, reserved. Four
// DWORDs, which is how the blob is laid out on disk.
constexpr size_t kFekHeaderSize = 16;

// Offset of the algorithm DWORD in the FEK header.
constexpr size_t kAlgorithmOffset = 8;

// Key sizes in bytes. DESX takes 128 bits that the cipher expands.
constexpr size_t kAes128KeySize = 16;
constexpr size_t kAes192KeySize = 24;
constexpr size_t kAes256KeySize = 32;
constexpr size_t k3DesKeySize = 24;
constexpr size_t kDesxFekKeySize = 16;

// The key length each cipher takes, in bytes. 3DES carries three 8-byte DES
// keys. DESX carries 128 bits, which the cipher expands into a DES key and two
// whitening keys.
[[nodiscard]] std::optional<size_t> KeyLengthOf(Algorithm algorithm) noexcept
{
  switch (algorithm)
  {
    case Algorithm::kAes128:
      return kAes128KeySize;
    case Algorithm::kAes192:
      return kAes192KeySize;
    case Algorithm::kAes256:
      return kAes256KeySize;
    case Algorithm::k3Des:
      return k3DesKeySize;
    case Algorithm::kDesx:
      return kDesxFekKeySize;
  }
  return std::nullopt;
}

// Reads the DWORD at "offset". The caller checked that it lies in "bytes".
[[nodiscard]] DWORD LoadDword(std::span<const BYTE> bytes,
                              size_t offset) noexcept
{
  DWORD value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}
}  // namespace

void SecureZero(std::span<BYTE> bytes) noexcept
{
  // The volatile write keeps the compiler from eliding the wipe.
  for (BYTE& byte : bytes)  // NOLINT(misc-const-correctness)
  {
    *static_cast<volatile BYTE*>(&byte) = 0;
  }
}

Fek::Fek(Algorithm algorithm, std::span<const BYTE> key)
    : algorithm_(algorithm), key_(key.begin(), key.end())
{
}

Fek::~Fek() { SecureZero(key_); }

std::optional<Fek> Fek::Parse(std::span<const BYTE> blob)
{
  if (blob.size() < kFekHeaderSize)
  {
    LogWarn("FEK blob is too short: {} bytes.", blob.size());
    return std::nullopt;
  }

  const auto algorithm =
      static_cast<Algorithm>(LoadDword(blob, kAlgorithmOffset));
  const std::optional<size_t> keyLength = KeyLengthOf(algorithm);
  if (!keyLength)
  {
    LogWarn("Unsupported EFS algorithm: 0x{:04X}.",
            static_cast<DWORD>(algorithm));
    return std::nullopt;
  }

  // The declared key length must be the one the cipher takes, and the key
  // must fit in the blob.
  if (LoadDword(blob, 0) != *keyLength ||
      blob.size() - kFekHeaderSize < *keyLength)
  {
    LogWarn("FEK key length does not match algorithm 0x{:04X}.",
            static_cast<DWORD>(algorithm));
    return std::nullopt;
  }

  return Fek(algorithm, blob.subspan(kFekHeaderSize, *keyLength));
}

Algorithm Fek::GetAlgorithm() const noexcept { return algorithm_; }

std::span<const BYTE> Fek::GetKey() const noexcept { return key_; }

}  // namespace NtfsBrowser::Efs
