#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <span>

#include <cryptopp/aes.h>
#include <cryptopp/des.h>
// Silences the weak-algorithm notice: MD5 is what the DESX key expansion uses.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include <cryptopp/md5.h>
#include <cryptopp/modes.h>

#include "efs/fek.h"
#include "efs/sector-cipher.h"
#include "ntfs-common.h"

namespace NtfsBrowser::Efs
{

namespace
{
// CBC over one Crypto++ block cipher, restarted with a fresh IV per sector.
template <class BlockCipher>
class CryptoPpDecryptor final : public SectorDecryptor
{
 public:
  explicit CryptoPpDecryptor(std::span<const BYTE> key)
  {
    cipher_.SetKey(key.data(), key.size());
  }

  bool DecryptSector(ULONGLONG offset, std::span<BYTE> sector) const override
  {
    if (sector.size() != kSectorSize)
    {
      return false;
    }

    try
    {
      const std::array<BYTE, kMaxBlockSize> initialization_vector =
          MakeSectorIv(offset, BlockCipher::BLOCKSIZE);
      CryptoPP::CBC_Mode_ExternalCipher::Decryption cbc(
          cipher_, initialization_vector.data());
      cbc.ProcessData(sector.data(), sector.data(), sector.size());
      return true;
    }
    catch (const std::exception& e)
    {
      LogException(e);
      return false;
    }
  }

 private:
  // Crypto++'s external-cipher mode takes a non-const cipher, though it
  // only reads the key schedule.
  mutable BlockCipher::Decryption cipher_;
};

template <class BlockCipher>
[[nodiscard]] std::unique_ptr<SectorDecryptor>
    MakeDecryptor(std::span<const BYTE> key)
{
  try
  {
    return std::make_unique<CryptoPpDecryptor<BlockCipher>>(key);
  }
  catch (const std::exception& e)
  {
    LogException(e);
    return nullptr;
  }
}

// Size of a DESX FEK, in bytes, and of the key Crypto++'s DES_XEX3 takes:
// two 8-byte whitening keys around an 8-byte DES key.
constexpr size_t kDesxFekSize = 16;
constexpr size_t kDesxKeySize = 24;

// The MD5 salts of the DESX key expansion. Each is 11 characters plus the
// terminating NUL, which is hashed too: 12 bytes. Taken from ntfs-3g.
constexpr std::array<BYTE, 12> kDesxSalt1{'D', 'a', 'n', ' ', 'S', 'i',
                                          'm', 'o', 'n', ' ', ' ', 0};
constexpr std::array<BYTE, 12> kDesxSalt2{'S', 'c', 'o', 't', 't', ' ',
                                          'F', 'i', 'e', 'l', 'd', 0};

// MD5 of the 128-bit DESX FEK followed by "salt".
[[nodiscard]] std::array<BYTE, CryptoPP::Weak::MD5::DIGESTSIZE>
    DesxDigest(std::span<const BYTE> fek, std::span<const BYTE> salt)
{
  std::array<BYTE, CryptoPP::Weak::MD5::DIGESTSIZE> digest{};
  CryptoPP::Weak::MD5 hash;
  hash.Update(fek.data(), fek.size());
  hash.Update(salt.data(), salt.size());
  hash.Final(digest.data());
  return digest;
}

// Expands the 128-bit DESX FEK into a Crypto++ DES_XEX3 key, as ntfs-3g does.
// The DES key is the first digest folded in half, 32-bit word against 32-bit
// word. The whitening keys are the halves of the second digest. DES_XEX3
// decrypts as: block ^ key[16..24), DES, then ^ key[0..8). So the output
// whitening goes last in the key, the input whitening first.
[[nodiscard]] std::array<BYTE, kDesxKeySize>
    ExpandDesxKey(std::span<const BYTE> fek)
{
  constexpr size_t kHalf = 8;
  // The digests are folded in 32-bit words.
  constexpr size_t kWord = 4;
  auto digest1 = DesxDigest(fek, kDesxSalt1);
  auto digest2 = DesxDigest(fek, kDesxSalt2);

  std::array<BYTE, kDesxKeySize> key{};
  const std::span<const BYTE> halves(digest2);
  std::ranges::copy(halves.subspan(kHalf), key.begin());
  for (size_t i = 0; i < kWord; ++i)
  {
    // i < 4: the highest digest1 index is 15 of 16, the highest key index 15.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    key[kHalf + i] = digest1[i] ^ digest1[kWord + i];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    key[kHalf + kWord + i] = digest1[2 * kWord + i] ^ digest1[3 * kWord + i];
  }
  std::ranges::copy(halves.first(kHalf),
                    std::span<BYTE>(key).subspan(2 * kHalf).begin());

  SecureZero(digest1);
  SecureZero(digest2);
  return key;
}
}  // namespace

std::unique_ptr<SectorDecryptor> MakeCryptoPpDecryptor(const Fek& fek)
{
  switch (fek.GetAlgorithm())
  {
    case Algorithm::kAes128:
    case Algorithm::kAes192:
    case Algorithm::kAes256:
      return MakeDecryptor<CryptoPP::AES>(fek.GetKey());
    case Algorithm::k3Des:
      return MakeDecryptor<CryptoPP::DES_EDE3>(fek.GetKey());
    case Algorithm::kDesx:
    {
      if (fek.GetKey().size() != kDesxFekSize)
      {
        return nullptr;
      }
      auto key = ExpandDesxKey(fek.GetKey());
      auto decryptor = MakeDecryptor<CryptoPP::DES_XEX3>(key);
      SecureZero(key);
      return decryptor;
    }
  }
  return nullptr;
}

}  // namespace NtfsBrowser::Efs
