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
    if (sector.size() != sector_size)
    {
      return false;
    }

    try
    {
      const std::array<BYTE, max_block_size> initialization_vector =
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
constexpr size_t desx_fek_size = 16;
constexpr size_t desx_key_size = 24;

// The MD5 salts of the DESX key expansion. Each is 11 characters plus the
// terminating NUL, which is hashed too: 12 bytes. Taken from ntfs-3g.
constexpr std::array<BYTE, 12> desx_salt1{'D', 'a', 'n', ' ', 'S', 'i',
                                          'm', 'o', 'n', ' ', ' ', 0};
constexpr std::array<BYTE, 12> desx_salt2{'S', 'c', 'o', 't', 't', ' ',
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
[[nodiscard]] std::array<BYTE, desx_key_size>
    ExpandDesxKey(std::span<const BYTE> fek)
{
  constexpr size_t half_value = 8;
  // The digests are folded in 32-bit words.
  constexpr size_t word_value = 4;
  auto digest1 = DesxDigest(fek, desx_salt1);
  auto digest2 = DesxDigest(fek, desx_salt2);

  std::array<BYTE, desx_key_size> key{};
  const std::span<const BYTE> halves(digest2);
  std::ranges::copy(halves.subspan(half_value), key.begin());
  for (size_t i = 0; i < word_value; ++i)
  {
    // i < 4: the highest digest1 index is 15 of 16, the highest key index 15.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    key[half_value + i] = digest1[i] ^ digest1[word_value + i];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    key[half_value + word_value + i] =
        digest1[2 * word_value + i] ^ digest1[3 * word_value + i];
  }
  std::ranges::copy(halves.first(half_value),
                    std::span<BYTE>(key).subspan(2 * half_value).begin());

  SecureZero(digest1);
  SecureZero(digest2);
  return key;
}
}  // namespace

std::unique_ptr<SectorDecryptor> MakeCryptoPpDecryptor(const Fek& fek)
{
  switch (fek.GetAlgorithm())
  {
    case Algorithm::Aes128:
    case Algorithm::Aes192:
    case Algorithm::Aes256:
      return MakeDecryptor<CryptoPP::AES>(fek.GetKey());
    case Algorithm::_3Des:
      return MakeDecryptor<CryptoPP::DES_EDE3>(fek.GetKey());
    case Algorithm::Desx:
    {
      if (fek.GetKey().size() != desx_fek_size)
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
