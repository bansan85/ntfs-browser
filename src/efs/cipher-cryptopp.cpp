#include <exception>

#include <cryptopp/aes.h>
#include <cryptopp/des.h>
#include <cryptopp/modes.h>

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
      const std::array<BYTE, kMaxBlockSize> iv = MakeSectorIv(offset);
      CryptoPP::CBC_Mode_ExternalCipher::Decryption cbc(cipher_, iv.data());
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
  mutable typename BlockCipher::Decryption cipher_;
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
      return MakeDecryptor<CryptoPP::DES_XEX3>(fek.GetKey());
  }
  return nullptr;
}

}  // namespace NtfsBrowser::Efs
