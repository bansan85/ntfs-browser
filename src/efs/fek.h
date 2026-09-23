#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/win-types.h>

#include "../internal-export.h"

namespace NtfsBrowser::Efs
{

// Overwrites key material in a way the compiler cannot drop as a dead store.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SecureZero(std::span<BYTE> bytes) noexcept;

// The symmetric cipher of an EFS file. The values are the CryptoAPI ALG_IDs
// that the FEK blob stores.
enum class Algorithm : DWORD
{
  k3Des = 0x6603,
  kDesx = 0x6604,
  kAes128 = 0x660E,
  kAes192 = 0x660F,
  kAes256 = 0x6610
};

// A File Encryption Key: a cipher and its key. Owns its bytes and wipes them
// on destruction.
class NTFS_BROWSER_EXPORT_TESTS_ONLY Fek
{
 public:
  Fek(const Fek& other) = delete;
  Fek& operator=(const Fek& other) = delete;
  Fek& operator=(Fek&& other) noexcept = delete;
  Fek(Fek&& other) noexcept = default;
  ~Fek();

  // Parses the blob an IEfsKeyProvider returns: a 16-byte header (key length,
  // entropy, algorithm, reserved), then the key. Returns nullopt for an
  // unknown algorithm, or a key length wrong for it. Copies the key.
  [[nodiscard]] static std::optional<Fek> Parse(std::span<const BYTE> blob);

  [[nodiscard]] Algorithm GetAlgorithm() const noexcept;
  [[nodiscard]] std::span<const BYTE> GetKey() const noexcept;

 private:
  Fek(Algorithm algorithm, std::span<const BYTE> key);

  Algorithm algorithm_;
  std::vector<BYTE> key_;
};

}  // namespace NtfsBrowser::Efs
