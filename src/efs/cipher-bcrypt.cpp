#ifdef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <bcrypt.h>
  #include <gsl/narrow>

  #include "efs/sector-cipher.h"
  #include "ntfs-common.h"

namespace NtfsBrowser::Efs
{

namespace
{
// BCrypt in CBC mode, restarted with a fresh IV per sector. Owns its
// algorithm provider and key handles.
class BCryptDecryptor final : public SectorDecryptor
{
 public:
  BCryptDecryptor() = default;
  BCryptDecryptor(BCryptDecryptor&& other) noexcept = delete;
  BCryptDecryptor(BCryptDecryptor const& other) = delete;
  BCryptDecryptor& operator=(BCryptDecryptor&& other) noexcept = delete;
  BCryptDecryptor& operator=(BCryptDecryptor const& other) = delete;

  ~BCryptDecryptor() override
  {
    if (key_ != nullptr)
    {
      BCryptDestroyKey(key_);
    }
    if (alg_ != nullptr)
    {
      BCryptCloseAlgorithmProvider(alg_, 0);
    }
  }

  // Opens the algorithm in CBC mode and imports the key. Returns false if
  // BCrypt refuses either.
  [[nodiscard]] bool Init(LPCWSTR algorithm_id, std::span<const BYTE> key,
                          size_t block_size)
  {
    block_size_ = block_size;
    if (!BCRYPT_SUCCESS(
            BCryptOpenAlgorithmProvider(&alg_, algorithm_id, nullptr, 0)))
    {
      return false;
    }

    // BCrypt wants the chaining mode as a wide string, terminator included.
    constexpr std::wstring_view cbc = BCRYPT_CHAIN_MODE_CBC;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(
            alg_, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(cbc.data())),
            static_cast<ULONG>((cbc.size() + 1) * sizeof(wchar_t)), 0)))
    {
      return false;
    }

    return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
        alg_, &key_, nullptr, 0, const_cast<PUCHAR>(key.data()),
        gsl::narrow<ULONG>(key.size()), 0));
  }

  bool DecryptSector(ULONGLONG offset, std::span<BYTE> sector) const override
  {
    if (sector.size() != sector_size)
    {
      return false;
    }

    // BCrypt advances the IV it is given, so it gets a copy.
    std::array<BYTE, max_block_size> iv = MakeSectorIv(offset, block_size_);
    ULONG produced = 0;
    const NTSTATUS status = BCryptDecrypt(
        key_, sector.data(), static_cast<ULONG>(sector.size()), nullptr,
        iv.data(), gsl::narrow<ULONG>(block_size_), sector.data(),
        static_cast<ULONG>(sector.size()), &produced, 0);
    return BCRYPT_SUCCESS(status) && produced == sector.size();
  }

 private:
  BCRYPT_ALG_HANDLE alg_{nullptr};
  BCRYPT_KEY_HANDLE key_{nullptr};
  size_t block_size_{0};
};

// The AES block size, in bytes.
constexpr size_t aes_block_size = 16;
}  // namespace

std::unique_ptr<SectorDecryptor> MakeBCryptDecryptor(const Fek& fek)
{
  LPCWSTR algorithm_id = nullptr;
  size_t block_size = 0;
  switch (fek.GetAlgorithm())
  {
    case Algorithm::Aes128:
    case Algorithm::Aes192:
    case Algorithm::Aes256:
      algorithm_id = BCRYPT_AES_ALGORITHM;
      block_size = aes_block_size;
      break;
    case Algorithm::_3Des:
      algorithm_id = BCRYPT_3DES_ALGORITHM;
      block_size = des_block_size;
      break;
    case Algorithm::Desx:
      return nullptr;
  }

  auto decryptor = std::make_unique<BCryptDecryptor>();
  if (!decryptor->Init(algorithm_id, fek.GetKey(), block_size))
  {
    LogWarn("BCrypt cannot use this FEK.");
    return nullptr;
  }
  return decryptor;
}

}  // namespace NtfsBrowser::Efs

#endif  // _WIN32
