#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>

#include <ntfs-browser/win-types.h>

#include "efs/fek.h"

namespace NtfsBrowser::Efs
{

// EFS encrypts each 512-byte sector on its own, whatever the sector size of
// the volume is.
inline constexpr size_t kSectorSize = 512;

// Widest cipher block, in bytes: AES. The DES family has 8-byte blocks.
inline constexpr size_t kMaxBlockSize = 16;

// The 16-byte CBC IV of a sector is two little-endian 64-bit words, each the
// sum of one of these constants and the byte offset of the sector in the
// stream. A DES-family cipher takes the first word only. Read off a real
// AES-256 file. The second word matches the constant published for EFS.
inline constexpr ULONGLONG kIvWord0 = 0x5816657be9161312ULL;
inline constexpr ULONGLONG kIvWord1 = 0x1989adbe44918961ULL;

// Builds the CBC IV of the sector that starts at byte offset "offset" of the
// stream. Only the first blockSize bytes matter to the caller.
[[nodiscard]] inline std::array<BYTE, kMaxBlockSize>
    MakeSectorIv(ULONGLONG offset) noexcept
{
  std::array<BYTE, kMaxBlockSize> iv{};
  const std::array<ULONGLONG, 2> words{kIvWord0 + offset, kIvWord1 + offset};
  for (size_t w = 0; w < words.size(); ++w)
  {
    for (size_t b = 0; b < sizeof(ULONGLONG); ++b)
    {
      iv[(w * sizeof(ULONGLONG)) + b] = static_cast<BYTE>(words[w] >> (8 * b));
    }
  }
  return iv;
}

// Decrypts EFS data one sector at a time. One instance holds one key.
class SectorDecryptor
{
 public:
  SectorDecryptor() = default;
  SectorDecryptor(SectorDecryptor&& other) noexcept = delete;
  SectorDecryptor(SectorDecryptor const& other) = delete;
  SectorDecryptor& operator=(SectorDecryptor&& other) noexcept = delete;
  SectorDecryptor& operator=(SectorDecryptor const& other) = delete;
  virtual ~SectorDecryptor() = default;

  // Decrypts, in place, the kSectorSize bytes of one sector. "offset" is the
  // byte offset of that sector in its stream. Returns false on a cipher error.
  [[nodiscard]] virtual bool DecryptSector(ULONGLONG offset,
                                           std::span<BYTE> sector) const = 0;
};

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
// The Crypto++ backend. Null if the key is unusable.
[[nodiscard]] std::unique_ptr<SectorDecryptor>
    MakeCryptoPpDecryptor(const Fek& fek);
#endif

#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
// The BCrypt backend. Null if the key is unusable, or for DESX, which BCrypt
// does not have.
[[nodiscard]] std::unique_ptr<SectorDecryptor>
    MakeBCryptDecryptor(const Fek& fek);
#endif

}  // namespace NtfsBrowser::Efs
